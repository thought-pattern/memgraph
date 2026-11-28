// Copyright 2026 Memgraph Ltd.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.txt; by using this file, you agree to be bound by the terms of the Business Source
// License, and you may not use this file except in compliance with the Business Source License.
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0, included in the file
// licenses/APL.txt.

#include "dbms/dbms_handler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <ranges>
#include <shared_mutex>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#include "dbms/constants.hpp"
#include "dbms/global.hpp"
#include "dbms/rpc.hpp"
#include "license/license.hpp"
#include "memory/global_memory_control.hpp"  // memory::PurgeUnusedMemory (synchronous reclaim on suspend)
#include "query/db_accessor.hpp"
#include "query/exceptions.hpp"
#include "spdlog/spdlog.h"
#include "storage/v2/inmemory/storage.hpp"
#include "utils/enum.hpp"
#include "utils/exceptions.hpp"
#include "utils/logging.hpp"
#include "utils/memory_tracker.hpp"  // utils::OutOfMemoryException (boot-OOM detection)
#include "utils/on_scope_exit.hpp"
#include "utils/synchronized.hpp"
#include "utils/uuid.hpp"

#include <mutex>

namespace memgraph::dbms {

namespace {
#ifdef MG_ENTERPRISE
constexpr std::string_view kDBPrefix = "database:";  // Key prefix for database durability
constexpr std::string_view kUuidKey = "uuid";        // JSON key for a durable entry's tenant uuid
constexpr std::string_view kRelDirKey = "rel_dir";   // JSON key for a durable entry's relative data dir
#endif

// Per storage
// NOTE Storage will connect to all replicas. Future work might change this
void RestoreReplication(replication::RoleMainData &mainData, DatabaseAccess db_acc) {
  spdlog::info("Restoring replication role.");

  // Each individual client has already been restored and started. Here we just go through each database and start its
  // client
  for (auto &instance_client : mainData.registered_replicas_) {
    spdlog::info("Replica {} restoration started for {}.", instance_client.name_, db_acc->name());
    auto client = std::make_unique<storage::ReplicationStorageClient>(instance_client, mainData.uuid_);
    auto *storage = db_acc->storage();
    auto protector = dbms::DatabaseProtector{db_acc};
    client->Start(storage, protector);
    // After start the storage <-> replica state should be READY or RECOVERING (if correctly started)
    // MAYBE_BEHIND isn't a statement of the current state, this is the default value
    // Failed to start due to branching of MAIN and REPLICA
    if (client->State() == storage::replication::ReplicaState::MAYBE_BEHIND) {
      spdlog::warn("Connection failed when registering replica {}. Replica will still be registered.",
                   instance_client.name_);
    }
    db_acc->storage()->repl_storage_state_.replication_storage_clients_.WithLock(
        [client = std::move(client)](auto &storage_clients) mutable { storage_clients.push_back(std::move(client)); });
    spdlog::info("Replica {} restored for {}.", instance_client.name_, db_acc->name());
  }
  spdlog::info("Replication role restored to MAIN.");
}
}  // namespace

#ifdef MG_ENTERPRISE
struct Durability {
  enum class DurabilityVersion : uint8_t {
    V0 = 0,
    V1,
    V2,  //!< hot/cold: COLD entries gain a `cold` marker + heartbeat metadata + cold_stats
  };

  struct VersionException : public utils::BasicException {
    VersionException() : utils::BasicException("Unsupported durability version!") {}
  };

  struct UnknownVersionException : public utils::BasicException {
    UnknownVersionException() : utils::BasicException("Unable to parse the durability version!") {}
  };

  struct MigrationException : public utils::BasicException {
    MigrationException() : utils::BasicException("Failed to migrate to the current durability version!") {}
  };

  static DurabilityVersion VersionCheck(std::optional<std::string_view> val) {
    if (!val) {
      return DurabilityVersion::V0;
    }
    if (val == "V1") {
      return DurabilityVersion::V1;
    }
    if (val == "V2") {
      return DurabilityVersion::V2;
    }
    throw UnknownVersionException();
  };

  static auto GenKey(std::string_view name) -> std::string { return fmt::format("{}{}", kDBPrefix, name); }

  static auto GenVal(utils::UUID uuid, std::filesystem::path rel_dir) {
    nlohmann::json json;
    json[kUuidKey] = uuid;
    json[kRelDirKey] = rel_dir;
    // TODO: Serialize the configuration
    return json.dump();
  }

  // Round-trippable JSON for a captured StorageInfo (hot/cold cold_stats). Distinct from
  // storage::ToJson (a lossy SHOW-presentation form): this stores every field by struct name so the
  // exact StorageInfo survives a restart. Driven by the single StorageInfoForEachField field list
  // (storage.hpp) so adding a field updates this path AND the V3 SLK wire at once. Enums persist as
  // their underlying integer: nlohmann's built-in enum (de)serialization is integer-based, and
  // storage::StorageMode's own to_json/from_json (storage_mode.hpp) keep that integer encoding while
  // range-checking the value on read. The human-readable string form lives only in storage::ToJson.
  static nlohmann::json StatsToJson(const storage::StorageInfo &s) {
    nlohmann::json j;
    storage::StorageInfoForEachField(s, [&](const char *key, const auto &v) { j[key] = v; });
    return j;
  }

  // Reads back a StatsToJson object. Tolerant of missing keys (defaults to a value-initialized field):
  // a V1 COLD entry never existed, but a forward-compatible read of a partial object must not throw.
  // A present-but-out-of-range storage_mode is range-checked by storage::from_json (falls back to
  // IN_MEMORY_TRANSACTIONAL); isolation/compression direct-cast via nlohmann's default enum from_json.
  static storage::StorageInfo StatsFromJson(const nlohmann::json &j) {
    storage::StorageInfo s{};
    storage::StorageInfoForEachField(
        s, [&](const char *key, auto &v) { v = j.value(key, std::remove_cvref_t<decltype(v)>{}); });
    return s;
  }

  // Durable entry for a COLD (suspended) tenant. Carries the HOT entry's identity (uuid, rel_dir)
  // plus the cold marker and the as-of-suspend cold_stats. On restart a `cold:true` entry is restored
  // as a no-value COLD shell (no storage build) — see the restore loop in the DbmsHandler ctor. A
  // resumed cold tenant trusts its own on-disk epoch (BuildDetached); no epoch is persisted here.
  static auto GenColdVal(utils::UUID uuid, std::filesystem::path rel_dir, const storage::StorageInfo &cold_stats)
      -> std::string {
    nlohmann::json json;
    json[kUuidKey] = uuid;
    json[kRelDirKey] = rel_dir;
    json["cold"] = true;
    json["cold_stats"] = StatsToJson(cold_stats);
    return json.dump();
  }

  static constexpr std::string_view kV0MigrationJournalPrefix = "v0_migration:";

  static void Migrate(kvstore::KVStore *durability, const std::filesystem::path &root) {
    const auto ver_val = durability->Get("version");
    const auto ver = VersionCheck(ver_val);

    std::map<std::string, std::string> to_put;
    std::vector<std::string> to_delete;

    // Update from V0 to V1
    if (ver == DurabilityVersion::V0) {
      std::vector<std::string> v0_keys;
      for (const auto &[key, _] : *durability) {
        if (key == "version" || key.starts_with(kV0MigrationJournalPrefix) || key.starts_with(kDBPrefix)) {
          continue;
        }
        v0_keys.push_back(key);
      }

      for (const auto &key : v0_keys) {
        const std::string journal_key = std::string{kV0MigrationJournalPrefix} + key;
        utils::UUID uuid;
        std::filesystem::path rel_dir;

        if (const auto existing_journal = durability->GetStrict(journal_key); existing_journal.has_value()) {
          try {
            const auto j = nlohmann::json::parse(*existing_journal);
            uuid = j.at(kUuidKey).get<utils::UUID>();
            rel_dir = j.at(kRelDirKey).get<std::filesystem::path>();
          } catch (const std::exception &) {
            throw MigrationException();
          }
        } else if (existing_journal.error() == kvstore::KVStore::GetError::kNotFound) {
          const auto target_path = (key == kDefaultDB) ? root : (root / kMultiTenantDir / std::string{uuid});
          rel_dir = std::filesystem::relative(target_path, root);
          const auto planned_val = GenVal(uuid, rel_dir);
          // Persist the planned target directory BEFORE renaming on disk so an interrupted upgrade recovers it.
          if (!durability->Put(journal_key, planned_val)) {
            throw MigrationException();
          }
        } else {
          throw MigrationException();
        }

        if (key != kDefaultDB) {
          const std::filesystem::path target_path = root / rel_dir;
          const std::filesystem::path old_dir = root / kMultiTenantDir / key;
          std::error_code ec;
          if (std::filesystem::exists(old_dir, ec)) {
            std::filesystem::rename(old_dir, target_path, ec);
            if (ec) {
              throw MigrationException();
            }
          } else if (!std::filesystem::exists(target_path, ec)) {
            throw MigrationException();
          }
        }

        to_put.emplace(GenKey(key), GenVal(uuid, rel_dir));
        to_delete.emplace_back(key);
        to_delete.emplace_back(journal_key);
      }
    }

    // Bump the version inside the SAME atomic batch as the key rewrite.
    to_put.emplace("version", "V2");
    if (!durability->PutAndDeleteMultiple(to_put, to_delete)) {
      throw MigrationException();
    }
  }
};

bool DbmsHandler::IsExpectedTenantRelDir_(std::string_view name, const utils::UUID &uuid,
                                          const std::filesystem::path &rel_dir) {
  if (rel_dir.empty() || rel_dir.is_absolute()) {
    return false;
  }
  for (const auto &part : rel_dir) {
    if (part == "..") {
      return false;
    }
  }
  const auto normalized = rel_dir.lexically_normal();
  if (name == kDefaultDB) {
    return normalized == std::filesystem::path{"."};
  }
  const auto expected = (std::filesystem::path{kMultiTenantDir} / std::string{uuid}).lexically_normal();
  return normalized == expected;
}

DbmsHandler::DbmsHandler(storage::Config config, ResumeRetryPolicy resume_retry_policy)
    : default_config_{std::move(config)}, resume_retry_policy_{resume_retry_policy} {
  const auto &root = default_config_.durability.storage_directory;
  storage::UpdatePaths(default_config_, root);
  const auto &db_dir = default_config_.durability.storage_directory / kMultiTenantDir;
  const auto durability_dir = db_dir / ".durability";
  utils::EnsureDirOrDie(db_dir);
  utils::EnsureDirOrDie(durability_dir);
  durability_ = std::make_unique<kvstore::KVStore>(durability_dir);

  Durability::Migrate(durability_.get(), root);
  tenant_profiles_ = std::make_unique<TenantProfiles>(*durability_);
  auto directories = std::set{std::string{kDefaultDB}};

  auto make_cold_entry =
      [&](std::string_view nm, utils::UUID id, std::filesystem::path rel_dir, const nlohmann::json &json) {
        SuspendedEntry entry;
        entry.salient = default_config_.salient;
        entry.salient.name = nm;
        entry.salient.uuid = id;
        entry.rel_dir = std::move(rel_dir);
        if (json.contains("cold_stats")) entry.cold_stats = Durability::StatsFromJson(json.at("cold_stats"));
        return entry;
      };

  bool durability_view_incomplete = false;
  auto it = durability_->begin(std::string(kDBPrefix));
  auto end = durability_->end(std::string(kDBPrefix));
  for (; it != end; ++it) {
    const auto &[key, config_json] = *it;
    const auto name = key.substr(kDBPrefix.size());
    nlohmann::json json;
    utils::UUID uuid;
    std::filesystem::path rel_dir;
    bool is_cold = false;
    try {
      json = nlohmann::json::parse(config_json);
      uuid = json.at(kUuidKey).get<utils::UUID>();
      rel_dir = json.at(kRelDirKey).get<std::filesystem::path>();
      if (json.contains("cold")) {
        is_cold = json.at("cold").get<bool>();
      }
    } catch (const nlohmann::json::exception &e) {
      durability_view_incomplete = true;
      spdlog::error(
          "Corrupt durable database entry '{}' ({}); skipping it. Its on-disk data is left untouched and "
          "directory cleanup is SKIPPED this boot so no data is deleted while the durable view is incomplete.",
          key,
          e.what());
      continue;
    }

    if (!IsExpectedTenantRelDir_(name, uuid, rel_dir)) {
      durability_view_incomplete = true;
      spdlog::error(
          "Durable database entry '{}' points to unexpected relative directory '{}' for UUID '{}'; refusing to open "
          "it and skipping directory cleanup this boot.",
          key,
          rel_dir.string(),
          std::string{uuid});
      continue;
    }

    directories.emplace(rel_dir.filename());

    if (is_cold) {
      spdlog::info("Restoring suspended (cold) database {} at {}.", name, rel_dir);
      try {
        db_handler_.EmplaceColdShell(name);
        suspended_.insert_or_assign(std::string{name}, make_cold_entry(name, uuid, rel_dir, json));
        spdlog::info("Suspended (cold) database {} restored.", name);
      } catch (const nlohmann::json::exception &e) {
        durability_view_incomplete = true;
        db_handler_.EraseColdShell(name);
        spdlog::warn(
            "Durable cold entry '{}' has a wrong-typed metadata field ({}); skipping it. "
            "Its on-disk data is left untouched and directory cleanup is SKIPPED this boot.",
            name,
            e.what());
      }
      continue;
    }

    const auto limit_lookup = tenant_profiles_->ResolveDatabaseLimit(name);
    if (!limit_lookup.has_value()) {
      durability_view_incomplete = true;
      spdlog::error(
          "Failed to resolve tenant profile memory limit for database '{}'; bringing it up in broken/unavailable "
          "state without loading its graph until its profile assignment and limit can be read.",
          name);
      auto placeholder_cfg = default_config_;
      placeholder_cfg.salient.name = name;
      placeholder_cfg.salient.uuid = uuid;
      placeholder_cfg.durability.recover_on_startup = false;
      placeholder_cfg.durability.snapshot_on_exit = false;
      placeholder_cfg.durability.snapshot_wal_mode = storage::Config::Durability::SnapshotWalMode::DISABLED;
      storage::UpdatePaths(placeholder_cfg, default_config_.durability.storage_directory / rel_dir);
      auto broken_db = New_(std::move(placeholder_cfg), /*txn=*/nullptr, /*persist_durability=*/false);
      MG_ASSERT(broken_db.has_value(), "Failed while materializing broken placeholder for database {}.", name);
      (*broken_db)->storage()->SetBroken(true);
      unreadable_profile_dbs_.insert_or_assign(
          std::string{name}, UnreadableProfileEntry{.salient = (*broken_db)->config().salient, .rel_dir = rel_dir});
      continue;
    }

    spdlog::info("Restoring database {} at {}.", name, rel_dir);
    try {
      auto new_db = New_(name, uuid, nullptr, rel_dir, *limit_lookup, /*persist_durability=*/false);
      MG_ASSERT(new_db.has_value(),
                "Failed while recovering database {} (error {}); aborting.",
                name,
                static_cast<int>(new_db.error()));
    } catch (const std::exception &e) {
      LOG_FATAL("Failed while recovering database {} ({}); aborting.", name, e.what());
    }
    spdlog::info("Database {} restored.", name);
  }
  if (it.HasError()) {
    durability_view_incomplete = true;
    spdlog::error(
        "Database catalog scan ended with a storage read error ({}); skipping unused-directory cleanup and profile "
        "pruning so live tenant directories are preserved.",
        it.ErrorMessage());
  }
  UpdateColdGauge_();

  if (durability_view_incomplete) {
    spdlog::warn(
        "Skipping unused-directory cleanup: a durable database entry failed to parse, so the set of live "
        "directories is incomplete and no data directory will be removed this boot.");
  } else {
    for (const auto &entry : std::filesystem::directory_iterator(db_dir)) {
      const auto &name = entry.path().filename().string();
      if (entry.is_directory() && !name.empty() && name.front() != '.') {
        auto itr = directories.find(name);
        if (itr == directories.end()) {
          std::error_code dummy;
          std::filesystem::remove_all(entry, dummy);
        } else {
          directories.erase(itr);
        }
      }
    }
  }

  SetupDefault_();

  {
    std::set<std::string> live{std::string{kDefaultDB}};
    auto pit = durability_->begin(std::string(kDBPrefix));
    const auto pend = durability_->end(std::string(kDBPrefix));
    for (; pit != pend; ++pit) {
      live.insert(pit->first.substr(kDBPrefix.size()));
    }
    if (pit.HasError() || durability_view_incomplete) {
      spdlog::warn("Skipping tenant profile attachment pruning because the durable database catalog scan was incomplete.");
    } else {
      const auto pruned = tenant_profiles_->PruneDatabases(live);
      if (pruned != 0) {
        spdlog::warn("Pruned {} tenant profile attachment(s) whose database no longer exists.", pruned);
      }
    }
  }

  RestoreTenantProfiles_();
}

bool DbmsHandler::TryReconcileUnreadableProfileDb_(std::string_view name) {
  if (!tenant_profiles_) return false;
  const auto limit_lookup = tenant_profiles_->ResolveDatabaseLimit(name);
  if (!limit_lookup.has_value()) {
    return false;
  }
  storage::SalientConfig salient;
  std::filesystem::path rel_dir;
  {
    auto wr = std::lock_guard{lock_};
    auto it = unreadable_profile_dbs_.find(name);
    if (it == unreadable_profile_dbs_.end()) {
      return true;
    }
    salient = it->second.salient;
    rel_dir = it->second.rel_dir;
    auto *gk = db_handler_.GetGatekeeper(name);
    if (!gk) {
      return false;
    }
    storage::Config cfg = default_config_;
    cfg.salient = salient;
    cfg.tenant_memory_limit = *limit_lookup;
    storage::UpdatePaths(cfg, default_config_.durability.storage_directory / rel_dir);
    // Stop background tasks and drop broken placeholder first so metrics registration for the same UUID does not collide.
    if (auto placeholder_acc = gk->access()) {
      placeholder_acc->get()->StopAllBackgroundTasks();
    }
    if (!db_handler_.TryDelete(name)) {
      return false;
    }
    auto recovered = New_(std::move(cfg), /*txn=*/nullptr, /*persist_durability=*/false);
    if (!recovered.has_value()) {
      return false;
    }
    unreadable_profile_dbs_.erase(it);
  }
  return true;
}

struct DropDatabase : memgraph::system::ISystemAction {
  explicit DropDatabase(utils::UUID uuid) : uuid_{uuid} {}

  void DoDurability() override { /* Done during DBMS execution */ }

  bool ShouldReplicateInCommunity() const override { return false; }

  bool DoReplication(replication::ReplicationClient &client, const utils::UUID &main_uuid,
                     memgraph::system::Transaction const &txn) const override {
    auto check_response = [](const storage::replication::DropDatabaseRes &response) {
      return response.result != storage::replication::DropDatabaseRes::Result::FAILURE;
    };

    return client.StreamAndFinalizeDelta<storage::replication::DropDatabaseRpc>(
        check_response, main_uuid, txn.last_committed_system_timestamp(), txn.timestamp(), uuid_);
  }

  void PostReplication(replication::RoleMainData &mainData) const override {}

 private:
  utils::UUID uuid_;
};

struct RenameDatabase : memgraph::system::ISystemAction {
  explicit RenameDatabase(utils::UUID uuid, std::string old_name, std::string new_name)
      : old_name_{std::move(old_name)}, new_name_{std::move(new_name)}, uuid_{uuid} {}

  void DoDurability() override { /* Done during DBMS execution */ }

  bool ShouldReplicateInCommunity() const override { return false; }

  bool DoReplication(replication::ReplicationClient &client, const utils::UUID &main_uuid,
                     memgraph::system::Transaction const &txn) const override {
    auto check_response = [](const storage::replication::RenameDatabaseRes &response) {
      return response.result != storage::replication::RenameDatabaseRes::Result::FAILURE;
    };

    return client.StreamAndFinalizeDelta<storage::replication::RenameDatabaseRpc>(
        check_response, main_uuid, txn.last_committed_system_timestamp(), txn.timestamp(), uuid_, old_name_, new_name_);
  }

  void PostReplication(replication::RoleMainData &mainData) const override {}

 private:
  std::string old_name_;
  std::string new_name_;
  utils::UUID uuid_;
};

std::optional<DbmsHandler::DeleteResult> DbmsHandler::TryDeleteColdFastPath_(std::string_view name,
                                                                             system::Transaction *transaction) {
  if (!suspended_.contains(name)) return std::nullopt;
  auto res = DeleteCold_(name);
  if (!res) return DeleteResult{std::unexpected{res.error()}};
  if (transaction) {
    transaction->AddAction<DropDatabase>(*res);
  }
  return DeleteResult{};
}

// Returns the DeleteError to propagate when the tenant is not live (name absent from items_):
// ALREADY_DROPPING if a husk with this name is still draining (IsPending), else NON_EXISTENT.
// A name erased from items_ by DeferDelete but still draining is a known husk (its DROPPING row
// shows in SHOW DATABASES); report that rather than the misleading NON_EXISTENT. Safe under lock_:
// IsPending() takes pending_mutex_ (lock_ -> pending_mutex_ is the only nesting direction).
// Caller invokes this only when GetConfig(db_name) already returned nullopt. Caller must hold lock_.
DeleteError DbmsHandler::NotLiveDeleteError_(std::string_view db_name) const {
  return db_handler_.IsPending(db_name) ? DeleteError::ALREADY_DROPPING : DeleteError::NON_EXISTENT;
}

void DbmsHandler::NotifyUuidRetired_(utils::UUID const &uuid, std::string_view name_for_log) {
  if (!on_uuid_retired_) return;
  // The retirement is already committed. An exception from the hook must
  // NOT surface as a false failure to the caller. If the hook throws, that uuid's parameter rows
  // stay orphaned — nothing reclaims them.
  try {
    on_uuid_retired_(uuid);
  } catch (const std::exception &e) {
    spdlog::error("on_uuid_retired_ threw after committed drop of '{}': {}", name_for_log, e.what());
  } catch (...) {
    spdlog::error("on_uuid_retired_ threw (non-std) after committed drop of '{}'", name_for_log);
  }
}

DbmsHandler::DeleteResult DbmsHandler::TryDelete(std::string db_name, system::Transaction *transaction) {
  auto wr = std::lock_guard{lock_};
  if (db_name == kDefaultDB) {
    return std::unexpected{DeleteError::DEFAULT_DB};
  }

  if (auto cold_res = TryDeleteColdFastPath_(db_name, transaction)) {
    return *cold_res;
  }

  const auto conf = db_handler_.GetConfig(db_name);
  if (!conf) return std::unexpected{NotLiveDeleteError_(db_name)};
  const auto storage_path = conf->durability.storage_directory;
  const auto uuid = conf->salient.uuid;
  const auto rel_dir = std::filesystem::relative(storage_path, default_config_.durability.storage_directory);

  auto *gk = db_handler_.GetGatekeeper(db_name);
  if (!gk) return std::unexpected{DeleteError::NON_EXISTENT};
  auto db_acc = gk->access();
  if (!db_acc || !gk->try_begin_suspend()) {
    return std::unexpected{DeleteError::USING};
  }

  // Confirm durable catalog and profile retirement BEFORE destroying the database or removing its directory.
  if (!DetachProfileAndRetireDurabilityKey_(db_name)) {
    gk->abort_suspend();
    return std::unexpected{DeleteError::DISK_FAIL};
  }

  db_acc->get()->StopAllBackgroundTasks();
  db_acc->get()->streams()->DropAll();
  db_acc.reset();
  gk->finish_suspend();
  db_handler_.EraseColdShell(db_name);
  unreadable_profile_dbs_.erase(db_name);

  if (IsExpectedTenantRelDir_(db_name, uuid, rel_dir)) {
    std::error_code ec;
    (void)std::filesystem::remove_all(storage_path, ec);
    if (ec) {
      spdlog::error(R"(Failed to clean disk while deleting database "{}" stored in {})", db_name, storage_path);
    }
  } else {
    spdlog::error(R"(Refusing to delete unexpected directory {} while dropping database "{}")", storage_path, db_name);
  }

  NotifyUuidRetired_(uuid, db_name);

  if (transaction) {
    transaction->AddAction<DropDatabase>(uuid);
  }

  return {};
}

DbmsHandler::DeleteResult DbmsHandler::Delete(std::string_view db_name, system::Transaction *transaction) {
  auto wr = std::lock_guard(lock_);

  // Cold-tenant fast path.
  if (auto cold_res = TryDeleteColdFastPath_(db_name, transaction)) {
    return *cold_res;
  }

  // Get DB config for the UUID and disk clean up
  const auto conf = db_handler_.GetConfig(db_name);
  if (!conf) return std::unexpected{NotLiveDeleteError_(db_name)};

  // Force delete
  const auto res = Delete_(std::string{db_name});
  if (res) {
    // Success; save delta
    if (transaction) {
      transaction->AddAction<DropDatabase>(conf->salient.uuid);
    }
  }
  return res;
}

DbmsHandler::DeleteResult DbmsHandler::Delete(std::string_view db_name) {
  auto wr = std::lock_guard(lock_);
  if (auto cold_res = TryDeleteColdFastPath_(db_name, /*transaction=*/nullptr)) {
    return *cold_res;
  }
  return Delete_(std::string{db_name});
}

DbmsHandler::DeleteResult DbmsHandler::Delete(utils::UUID uuid) {
  auto wr = std::lock_guard(lock_);
  // COLD first, matching the rest of the Delete/TryDelete family (a DROP treats a COLD tenant as an
  // equally valid target, unlike the Get_ read path which privileges HOT to surface a "RESUME it"
  // error). Ordering is correctness-neutral: under lock_ a tenant is HOT xor in suspended_, never both
  // (Resume_ publishes HOT and erases the suspended_ entry in one nothrow scope). Both lookups are
  // in-memory map scans — neither touches disk. (Replica apply path — no transaction arg.)
  if (auto it = FindSuspendedByUuid_(uuid); it != suspended_.end()) {
    auto dropped = DeleteCold_(it->first);
    if (!dropped) return std::unexpected{dropped.error()};
    return {};
  }
  auto it = FindHotByUuid_(uuid);
  if (it == db_handler_.end()) return std::unexpected{DeleteError::NON_EXISTENT};
  return Delete_(it->first);
}

DbmsHandler::RenameResult DbmsHandler::Rename(std::string_view old_name, std::string_view new_name,
                                              system::Transaction *txn) {
  auto wr = std::lock_guard{lock_};

  // Check if trying to rename default database
  if (old_name == kDefaultDB) {
    return std::unexpected{RenameError::DEFAULT_DB};
  }

  if (old_name == new_name) {
    return std::unexpected{RenameError::SAME_NAME};
  }

  // A COLD (suspended) tenant cannot be renamed. Its gatekeeper is a no-value shell, so
  // db_handler_.Rename would move the shell and the subsequent Get(new_name) MG_ASSERT would abort
  // the process. Renaming a suspended tenant is not supported — RESUME it first.
  if (suspended_.contains(old_name)) {
    return std::unexpected{RenameError::SUSPENDED};
  }

  // A tenant mid-transition must not be renamed either. Suspend_ holds a raw pointer to the in-map
  // gatekeeper across its lock-free phase; db_handler_.Rename would move-erase that node out from
  // under it (use-after-free, or the post-rename Get MG_ASSERT). The suspended_ check above already
  // covers COLD and RESUMING (their suspended_ entry is live), so this closes the remaining SUSPENDING
  // window. Mirrors the DROP sibling DeleteCold_, which likewise refuses a non-HOT gatekeeper under
  // this same exclusive lock_. Safety here is otherwise external (all DDL is serialized by the system
  // transaction) — this makes it local and robust to any future finer-grained DDL locking.
  if (auto *gk = db_handler_.GetGatekeeper(old_name); gk && gk->state() != utils::GatekeeperState::HOT) {
    return std::unexpected{RenameError::USING};
  }

  std::optional<std::string> old_durable_val;
  std::string old_key;
  std::string new_key;
  if (durability_) {
    old_key = Durability::GenKey(old_name);
    new_key = Durability::GenKey(new_name);
    auto fetched = durability_->GetStrict(old_key);
    if (!fetched.has_value()) {
      spdlog::error("Failed to read durable catalog entry '{}' before renaming database to '{}'.", old_key, new_name);
      return std::unexpected{fetched.error() == kvstore::KVStore::GetError::kNotFound ? RenameError::NON_EXISTENT
                                                                                      : RenameError::FAIL};
    }
    old_durable_val = std::move(*fetched);
  }

  // Pre-allocate rollback name so reversing an in-memory rename on durability failure cannot fail.
  std::string rollback_old_name{old_name};

  if (auto rename_result = db_handler_.Rename(old_name, new_name); !rename_result.has_value()) {
    return std::unexpected{rename_result.error()};
  }

  auto new_db = db_handler_.Get(new_name);
  MG_ASSERT(new_db, "Database {} not found after rename.", new_name);
  const auto db_uuid = (*new_db)->uuid();
  (*new_db)->Rename(new_name);

  if (durability_) {
    const std::map<std::string, std::string> extra_puts{{new_key, *old_durable_val}};
    const std::vector<std::string> extra_deletes{old_key};
    bool committed = false;
    if (tenant_profiles_) {
      const auto renamed = tenant_profiles_->RenameDatabase(old_name, new_name, extra_puts, extra_deletes);
      committed = renamed.has_value();
    } else {
      committed = durability_->PutAndDeleteMultiple(extra_puts, extra_deletes);
    }
    if (!committed) {
      spdlog::error("Failed to atomically persist rename of database {} to {}; rolling back in-memory rename.",
                    old_name,
                    new_name);
      // NOLINTNEXTLINE(readability-suspicious-call-argument)
      [[maybe_unused]] auto rolled_back = db_handler_.Rename(new_name, rollback_old_name);
      (*new_db)->Rename(rollback_old_name);
      return std::unexpected{RenameError::FAIL};
    }
  } else if (tenant_profiles_) {
    const auto renamed = tenant_profiles_->RenameDatabase(old_name, new_name);
    if (!renamed.has_value() && renamed.error() == TenantProfiles::RenameError::DURABILITY_ERROR) {
      // NOLINTNEXTLINE(readability-suspicious-call-argument)
      [[maybe_unused]] auto rolled_back = db_handler_.Rename(new_name, rollback_old_name);
      (*new_db)->Rename(rollback_old_name);
      return std::unexpected{RenameError::FAIL};
    }
  }

  if (auto up_it = unreadable_profile_dbs_.find(old_name); up_it != unreadable_profile_dbs_.end()) {
    auto moved_entry = std::move(up_it->second);
    moved_entry.salient.name = new_name;
    unreadable_profile_dbs_.erase(up_it);
    unreadable_profile_dbs_.insert_or_assign(std::string{new_name}, std::move(moved_entry));
  }

  if (txn) {
    txn->AddAction<RenameDatabase>(db_uuid, std::string{old_name}, std::string{new_name});
  }

  return {};  // Success
}

DbmsHandler::ReplicaRenameOutcome DbmsHandler::RenameForReplication(const utils::UUID &expected_uuid,
                                                                    std::string_view old_name,
                                                                    std::string_view new_name) {
  auto wr = std::lock_guard{lock_};
  if (old_name == kDefaultDB || old_name == new_name) {
    return ReplicaRenameOutcome::FAILURE;
  }
  if (suspended_.contains(old_name)) {
    return ReplicaRenameOutcome::FAILURE;
  }

  if (auto old_acc = db_handler_.Get(old_name)) {
    if ((*old_acc)->uuid() != expected_uuid) {
      spdlog::warn("RenameForReplication: database '{}' has UUID '{}', expected '{}'; rejecting.",
                   old_name,
                   std::string{(*old_acc)->uuid()},
                   std::string{expected_uuid});
      return ReplicaRenameOutcome::FAILURE;
    }
    old_acc->reset();
    if (auto *gk = db_handler_.GetGatekeeper(old_name); !gk || gk->state() != utils::GatekeeperState::HOT) {
      return ReplicaRenameOutcome::FAILURE;
    }

    std::optional<std::string> old_durable_val;
    std::string old_key;
    std::string new_key;
    if (durability_) {
      old_key = Durability::GenKey(old_name);
      new_key = Durability::GenKey(new_name);
      auto fetched = durability_->GetStrict(old_key);
      if (!fetched.has_value()) {
        return ReplicaRenameOutcome::FAILURE;
      }
      old_durable_val = std::move(*fetched);
    }

    std::string rollback_old_name{old_name};
    if (!db_handler_.Rename(old_name, new_name).has_value()) {
      return ReplicaRenameOutcome::FAILURE;
    }
    auto new_db = db_handler_.Get(new_name);
    MG_ASSERT(new_db, "Database {} not found after replicated rename.", new_name);
    (*new_db)->Rename(new_name);

    if (durability_) {
      const std::map<std::string, std::string> extra_puts{{new_key, *old_durable_val}};
      const std::vector<std::string> extra_deletes{old_key};
      const bool committed = tenant_profiles_
                                 ? tenant_profiles_->RenameDatabase(old_name, new_name, extra_puts, extra_deletes)
                                       .has_value()
                                 : durability_->PutAndDeleteMultiple(extra_puts, extra_deletes);
      if (!committed) {
        // NOLINTNEXTLINE(readability-suspicious-call-argument)
        [[maybe_unused]] auto rolled_back = db_handler_.Rename(new_name, rollback_old_name);
        (*new_db)->Rename(rollback_old_name);
        return ReplicaRenameOutcome::FAILURE;
      }
    } else if (tenant_profiles_) {
      const auto renamed = tenant_profiles_->RenameDatabase(old_name, new_name);
      if (!renamed.has_value() && renamed.error() == TenantProfiles::RenameError::DURABILITY_ERROR) {
        // NOLINTNEXTLINE(readability-suspicious-call-argument)
        [[maybe_unused]] auto rolled_back = db_handler_.Rename(new_name, rollback_old_name);
        (*new_db)->Rename(rollback_old_name);
        return ReplicaRenameOutcome::FAILURE;
      }
    }

    if (auto up_it = unreadable_profile_dbs_.find(old_name); up_it != unreadable_profile_dbs_.end()) {
      auto moved_entry = std::move(up_it->second);
      moved_entry.salient.name = new_name;
      unreadable_profile_dbs_.erase(up_it);
      unreadable_profile_dbs_.insert_or_assign(std::string{new_name}, std::move(moved_entry));
    }
    return ReplicaRenameOutcome::SUCCESS;
  }

  if (auto *gk = db_handler_.GetGatekeeper(old_name); gk != nullptr) {
    return ReplicaRenameOutcome::FAILURE;
  }
  if (auto new_acc = db_handler_.Get(new_name); new_acc && (*new_acc)->uuid() == expected_uuid) {
    return ReplicaRenameOutcome::NO_NEED;
  }
  return ReplicaRenameOutcome::FAILURE;
}

struct CreateDatabase : memgraph::system::ISystemAction {
  explicit CreateDatabase(storage::SalientConfig config, DatabaseAccess db_acc)
      : config_{std::move(config)}, db_acc(db_acc) {}

  void DoDurability() override {
    // Done during dbms execution
  }

  bool ShouldReplicateInCommunity() const override { return false; }

  bool DoReplication(replication::ReplicationClient &client, const utils::UUID &main_uuid,
                     memgraph::system::Transaction const &txn) const override {
    auto check_response = [](const storage::replication::CreateDatabaseRes &response) {
      return response.result != storage::replication::CreateDatabaseRes::Result::FAILURE;
    };

    return client.StreamAndFinalizeDelta<storage::replication::CreateDatabaseRpc>(
        check_response, main_uuid, txn.last_committed_system_timestamp(), txn.timestamp(), config_);
  }

  void PostReplication(replication::RoleMainData &mainData) const override {
    // Sync database with REPLICAs
    // NOTE: The function bellow is used to create ReplicationStorageClient, so it must be called on a new storage
    // We don't need to have it here, since the function won't fail even if the replication client fails to
    // connect We will just have everything ready, for recovery at some point.
    dbms::DbmsHandler::RecoverStorageReplication(db_acc, mainData);
  }

 private:
  storage::SalientConfig config_;
  DatabaseAccess db_acc;
};

constexpr int64_t kUnusedMemoryLimit = 0;

struct TenantProfileAction : memgraph::system::ISystemAction {
  using Action = storage::replication::TenantProfileReq::Action;

  TenantProfileAction(Action action, std::string_view profile_name, std::string_view db_name, int64_t memory_limit)
      : action_{action}, profile_{.name = std::string{profile_name}, .memory_limit = memory_limit}, db_name_{db_name} {}

  void DoDurability() override {}

  bool ShouldReplicateInCommunity() const override { return false; }

  bool DoReplication(replication::ReplicationClient &client, const utils::UUID &main_uuid,
                     memgraph::system::Transaction const &txn) const override {
    return client.StreamAndFinalizeDelta<storage::replication::TenantProfileRpc>(
        [](const storage::replication::TenantProfileRes &response) { return response.success; },
        main_uuid,
        txn.last_committed_system_timestamp(),
        txn.timestamp(),
        action_,
        profile_,
        db_name_);
  }

  void PostReplication(replication::RoleMainData & /*mainData*/) const override {}

 private:
  Action action_;
  TenantProfiles::Profile profile_;
  std::string db_name_;
};

std::expected<void, TenantProfiles::CreateError> DbmsHandler::CreateTenantProfile(std::string_view name,
                                                                                  int64_t memory_limit,
                                                                                  system::Transaction *sys_txn) {
  auto result = tenant_profiles_->Create(name, memory_limit);
  if (!result) return std::unexpected{result.error()};
  if (sys_txn) {
    sys_txn->AddAction<TenantProfileAction>(TenantProfileAction::Action::CREATE, name, "", memory_limit);
  }
  return {};
}

std::expected<void, TenantProfiles::AlterError> DbmsHandler::AlterTenantProfile(std::string_view name,
                                                                                int64_t memory_limit,
                                                                                system::Transaction *sys_txn) {
  auto result = tenant_profiles_->Alter(name, memory_limit);
  if (!result) return std::unexpected{result.error()};
  for (const auto &db_name : *result) {
    (void)TryReconcileUnreadableProfileDb_(db_name);
    try {
      auto db_acc = Get(db_name);
      db_acc.get()->SetTenantMemoryLimit(memory_limit);
    } catch (const SuspendedDatabaseException &) {
      spdlog::info("AlterTenantProfile: database '{}' is suspended — updated limit will apply on resume", db_name);
    } catch (const UnknownDatabaseException &) {
      spdlog::warn("AlterTenantProfile: database '{}' not found while applying profile '{}' — skipping", db_name, name);
    }
  }
  if (sys_txn) {
    sys_txn->AddAction<TenantProfileAction>(TenantProfileAction::Action::ALTER, name, "", memory_limit);
  }
  return {};
}

std::expected<void, TenantProfiles::DropError> DbmsHandler::DropTenantProfile(std::string_view name,
                                                                              system::Transaction *sys_txn) {
  auto result = tenant_profiles_->Drop(name);
  if (!result) return std::unexpected{result.error()};
  if (sys_txn) {
    sys_txn->AddAction<TenantProfileAction>(TenantProfileAction::Action::DROP, name, "", kUnusedMemoryLimit);
  }
  return {};
}

std::expected<void, TenantProfiles::AttachError> DbmsHandler::SetTenantProfileOnDatabase(std::string_view profile_name,
                                                                                         std::string_view db_name,
                                                                                         system::Transaction *sys_txn) {
  {
    auto rd = std::shared_lock{lock_};
    if (!db_handler_.Has(db_name) && !suspended_.contains(db_name)) {
      throw UnknownDatabaseException("Tried to retrieve an unknown database \"{}\".", db_name);
    }
  }
  auto result = tenant_profiles_->AttachToDatabase(profile_name, db_name);
  if (!result) return std::unexpected{result.error()};
  (void)TryReconcileUnreadableProfileDb_(db_name);
  try {
    auto db_acc = Get(db_name);
    db_acc.get()->SetTenantMemoryLimit(*result);
  } catch (const SuspendedDatabaseException &) {
    // Suspended tenant: Resume_ resolves and installs the attached profile's memory limit before recovery.
  } catch (const UnknownDatabaseException &) {
  }
  if (sys_txn) {
    sys_txn->AddAction<TenantProfileAction>(
        TenantProfileAction::Action::SET_ON_DATABASE, profile_name, db_name, *result);
  }
  return {};
}

std::expected<void, TenantProfiles::DetachError> DbmsHandler::RemoveTenantProfileFromDatabase(
    std::string_view db_name, system::Transaction *sys_txn) {
  {
    auto rd = std::shared_lock{lock_};
    if (!db_handler_.Has(db_name) && !suspended_.contains(db_name)) {
      throw UnknownDatabaseException("Tried to retrieve an unknown database \"{}\".", db_name);
    }
  }
  auto result = tenant_profiles_->DetachFromDatabase(db_name);
  if (!result) return std::unexpected{result.error()};
  (void)TryReconcileUnreadableProfileDb_(db_name);
  try {
    auto db_acc = Get(db_name);
    db_acc.get()->SetTenantMemoryLimit(0);
  } catch (const SuspendedDatabaseException &) {
    // Suspended tenant: durable detachment succeeded; Resume_ will observe no attached profile on resume.
  } catch (const UnknownDatabaseException &) {
  }
  if (sys_txn) {
    sys_txn->AddAction<TenantProfileAction>(
        TenantProfileAction::Action::REMOVE_FROM_DATABASE, "", db_name, kUnusedMemoryLimit);
  }
  return {};
}

bool DbmsHandler::ReconcileTenantProfilesForRecovery(const std::vector<TenantProfiles::Profile> &incoming) {
  if (!tenant_profiles_) return true;
  if (!tenant_profiles_->ReconcileForRecovery(incoming)) {
    return false;
  }

  std::unordered_map<std::string, int64_t> desired_limits;
  for (const auto &profile : incoming) {
    for (const auto &db_name : profile.databases) {
      desired_limits[db_name] = profile.memory_limit;
    }
  }

  std::vector<std::string> unreadable_names;
  {
    auto rd = std::shared_lock{lock_};
    for (const auto &[name, _] : unreadable_profile_dbs_) {
      unreadable_names.push_back(name);
    }
  }
  for (const auto &name : unreadable_names) {
    (void)TryReconcileUnreadableProfileDb_(name);
  }

  auto rd = std::shared_lock{lock_};
  ForEachLocked_([&](DatabaseAccess db_acc) {
    const auto it = desired_limits.find(db_acc->name());
    db_acc->SetTenantMemoryLimit(it != desired_limits.end() ? it->second : 0);
  });
  return true;
}

DbmsHandler::NewResultT DbmsHandler::New_(storage::Config storage_config, system::Transaction *txn,
                                          bool persist_durability) {
  const std::string created_name{*storage_config.salient.name.str_view()};
  const auto created_dir = storage_config.durability.storage_directory;
  auto new_db = db_handler_.New(storage_config);

  if (new_db) {
    if (persist_durability && !UpdateDurability(storage_config)) {
      spdlog::error("Failed to persist catalog entry for newly created database '{}'; rolling back creation.",
                    created_name);
      (*new_db)->StopAllBackgroundTasks();
      new_db->reset();
      (void)db_handler_.TryDelete(created_name);
      if (created_name != kDefaultDB) {
        const auto rel_dir = std::filesystem::relative(created_dir, default_config_.durability.storage_directory);
        if (IsExpectedTenantRelDir_(created_name, storage_config.salient.uuid, rel_dir)) {
          std::error_code ec;
          (void)std::filesystem::remove_all(created_dir, ec);
        }
      }
      return std::unexpected{NewError::GENERIC};
    }
    if (ttl_user_check_) (*new_db)->storage()->ttl_.SetUserCheck(ttl_user_check_);
    if (txn) {
      txn->AddAction<CreateDatabase>(storage_config.salient, new_db.value());
    }
  }
  return new_db;
}

std::expected<utils::UUID, DeleteError> DbmsHandler::DeleteCold_(std::string_view name) {
  auto it = suspended_.find(name);
  if (it == suspended_.end()) return std::unexpected{DeleteError::NON_EXISTENT};

  auto *gk = db_handler_.GetGatekeeper(name);
  if (!gk || gk->state() != utils::GatekeeperState::COLD) {
    return std::unexpected{DeleteError::USING};
  }

  const auto uuid = it->second.salient.uuid;
  const auto rel_dir = it->second.rel_dir;
  const auto data_dir = default_config_.durability.storage_directory / rel_dir;
  const std::string name_copy{name};

  if (!DetachProfileAndRetireDurabilityKey_(name_copy)) {
    return std::unexpected{DeleteError::DISK_FAIL};
  }

  suspended_.erase(it);
  unreadable_profile_dbs_.erase(name_copy);
  db_handler_.EraseColdShell(name_copy);
  if (IsExpectedTenantRelDir_(name_copy, uuid, rel_dir)) {
    std::error_code ec;
    (void)std::filesystem::remove_all(data_dir, ec);
    if (ec) {
      spdlog::error(
          R"(Failed to clean disk while dropping suspended database "{}" at {})", name_copy, data_dir.string());
    }
  } else {
    spdlog::error(R"(Refusing to delete unexpected directory {} while dropping suspended database "{}")",
                  data_dir.string(),
                  name_copy);
  }
  NotifyUuidRetired_(uuid, name_copy);
  UpdateColdGauge_();
  return uuid;
}

DbmsHandler::DeleteResult DbmsHandler::Delete_(std::string db_name) {
  if (db_name == kDefaultDB) {
    return std::unexpected{DeleteError::DEFAULT_DB};
  }

  const auto storage_path = StorageDir_(db_name);
  if (!storage_path) return std::unexpected{DeleteError::NON_EXISTENT};

  std::optional<utils::UUID> db_uuid;
  {
    auto db = db_handler_.Get(db_name);
    if (!db) {
      auto *gk = db_handler_.GetGatekeeper(db_name);
      if (gk && gk->state() != utils::GatekeeperState::HOT) return std::unexpected{DeleteError::USING};
      return std::unexpected{DeleteError::NON_EXISTENT};
    }
    db_uuid = db->get()->uuid();
  }

  const auto rel_dir = std::filesystem::relative(*storage_path, default_config_.durability.storage_directory);
  const bool valid_rel_dir = IsExpectedTenantRelDir_(db_name, *db_uuid, rel_dir);

  // Allocate all deferred-handoff objects BEFORE retiring the durable catalog entry so an allocation
  // failure leaves the tenant HOT, unsealed, and durably registered.
  auto husk_id = std::string{*db_uuid};
  std::move_only_function<void(Database &)> stop_step = [](Database &db) {
    db.StopAllBackgroundTasks();
    db.streams()->DropAll();
  };
  std::move_only_function<void()> post_delete_step =
      [storage_path = *storage_path, db_name = db_name, valid_rel_dir]() {
        if (!valid_rel_dir) {
          spdlog::error(R"(Refusing to delete unexpected directory {} while deleting database "{}")",
                        storage_path,
                        db_name);
          return;
        }
        std::error_code ec;
        (void)std::filesystem::remove_all(storage_path, ec);
        if (ec) {
          spdlog::error(R"(Failed to clean disk while deleting database "{}" stored in {})", db_name, storage_path);
        }
      };
  auto husk_name = db_name;

  if (!DetachProfileAndRetireDurabilityKey_(db_name)) {
    return std::unexpected{DeleteError::DISK_FAIL};
  }
  unreadable_profile_dbs_.erase(db_name);

  auto *gk = db_handler_.GetGatekeeper(db_name);
  MG_ASSERT(gk, "Delete_: gatekeeper for '{}' vanished under lock_", db_name);
  gk->seal();
  db_handler_.DeferDelete(std::move(husk_name), std::move(husk_id), std::move(stop_step), std::move(post_delete_step));

  NotifyUuidRetired_(*db_uuid, db_name);

  return {};  // Success
}

bool DbmsHandler::DetachProfileAndRetireDurabilityKey_(std::string_view db_name) {
  if (tenant_profiles_) {
    const auto detach_res = tenant_profiles_->DetachFromDatabase(db_name, {Durability::GenKey(db_name)});
    if (detach_res.has_value()) {
      return true;
    }
    if (detach_res.error() != TenantProfiles::DetachError::NOT_ATTACHED) {
      spdlog::error("Failed to detach tenant profile and retire catalog entry for database '{}'.", db_name);
      return false;
    }
  }
  if (!durability_) return true;
  if (!durability_->Delete(Durability::GenKey(db_name))) {
    spdlog::error("Failed to delete durable catalog entry for database '{}'.", db_name);
    return false;
  }
  return true;
}

bool DbmsHandler::UpdateDurability(const storage::Config &config, std::optional<std::filesystem::path> rel_dir) {
  if (!durability_) return true;
  const auto &key = Durability::GenKey(*config.salient.name.str_view());
  if (rel_dir == std::nullopt) {
    rel_dir =
        std::filesystem::relative(config.durability.storage_directory, default_config_.durability.storage_directory);
  }
  const auto &val = Durability::GenVal(config.salient.uuid, *rel_dir);
  return durability_->Put(key, val);
}

#endif

std::optional<memgraph::metrics::StorageSnapshot> DbmsHandler::TryGetStorageSnapshotForMetrics(utils::UUID const &uuid
                                                                                               [[maybe_unused]]) {
#ifdef MG_ENTERPRISE
  try {
    auto rd = std::shared_lock{lock_};
    auto db = Get_(uuid);
    auto const info = db->storage()->GetBaseInfo();
    return memgraph::metrics::StorageSnapshot{.vertex_count = info.vertex_count,
                                              .edge_count = info.edge_count,
                                              .disk_usage = info.disk_usage,
                                              .db_memory_tracked = db->DbMemoryUsage(),
                                              .db_peak_memory_tracked = db->DbPeakMemoryUsage(),
                                              .db_storage_memory_tracked = db->DbStorageMemoryUsage(),
                                              .db_embedding_memory_tracked = db->DbEmbeddingMemoryUsage(),
                                              .db_query_memory_tracked = db->DbQueryMemoryUsage()};
  } catch (UnknownDatabaseException const &) {
    return std::nullopt;
  }
#else
  auto db_opt = db_gatekeeper_.access();
  if (!db_opt) return std::nullopt;
  auto const info = (*db_opt)->storage()->GetBaseInfo();
  return memgraph::metrics::StorageSnapshot{.vertex_count = info.vertex_count,
                                            .edge_count = info.edge_count,
                                            .disk_usage = info.disk_usage,
                                            .db_memory_tracked = (*db_opt)->DbMemoryUsage(),
                                            .db_peak_memory_tracked = (*db_opt)->DbPeakMemoryUsage(),
                                            .db_storage_memory_tracked = (*db_opt)->DbStorageMemoryUsage(),
                                            .db_embedding_memory_tracked = (*db_opt)->DbEmbeddingMemoryUsage(),
                                            .db_query_memory_tracked = (*db_opt)->DbQueryMemoryUsage()};
#endif
}

void DbmsHandler::RecoverStorageReplication(DatabaseAccess db_acc, replication::RoleMainData &role_main_data) {
  auto const is_enterprise = license::global_license_checker.IsEnterpriseValidFast();
  if (is_enterprise || db_acc->name() == dbms::kDefaultDB) {
    // Handle global replication state
    spdlog::info("Replication configuration will be stored and will be automatically restored in case of a crash.");
    // RECOVER REPLICA CONNECTIONS
    memgraph::dbms::RestoreReplication(role_main_data, db_acc);
  } else if (!role_main_data.registered_replicas_.empty()) {
    spdlog::warn("Multi-tenant replication is currently not supported!");
  }
}

void DbmsHandler::RestoreTriggers(query::InterpreterContext *ic) {
#ifdef MG_ENTERPRISE
  auto wr = std::lock_guard{lock_};
  for (auto &[_, db_gk] : db_handler_) {
#else
  {
    auto &db_gk = db_gatekeeper_;
#endif
    if (auto db_acc_opt = db_gk.access()) {
      auto &db_acc = *db_acc_opt;
      spdlog::debug("Restoring trigger for database \"{}\"", db_acc->name());
      auto storage_accessor = db_acc->Access(memgraph::storage::WRITE);
      auto dba = memgraph::query::DbAccessor{storage_accessor.get()};
      db_acc->trigger_store()->RestoreTriggers(
          &ic->ast_cache, &dba, ic->config.query, ic->auth_checker, db_acc->name(), ic->parameters);
    }
  }
}

void DbmsHandler::RestoreTriggersFor(DatabaseAccess db_acc, query::InterpreterContext *ic) {
  // Single-db trigger restore for the hot/cold resume arm (mirrors the per-db body of RestoreTriggers but
  // on a caller-held accessor, with no lock_). The resumed Database's TriggerStore is constructed empty by
  // BuildDetached (the ctor does not auto-arm), so this re-parses + arms the durably-persisted triggers.
  auto storage_accessor = db_acc->Access(memgraph::storage::WRITE);
  auto dba = memgraph::query::DbAccessor{storage_accessor.get()};
  db_acc->trigger_store()->RestoreTriggers(
      &ic->ast_cache, &dba, ic->config.query, ic->auth_checker, db_acc->name(), ic->parameters);
}

#ifdef MG_ENTERPRISE
// Hot/cold SUSPEND as a system action. Recording it makes SUSPEND participate in the system
// transaction — serialized + system-timestamped exactly like CREATE/DROP DATABASE. DoReplication
// streams a SuspendDatabaseRpc to each replica, which applies its own teardown (in system-timestamp
// order) so the replica's copy is torn down to a COLD shell to match MAIN.
struct SuspendDatabase : memgraph::system::ISystemAction {
  explicit SuspendDatabase(utils::UUID uuid) : uuid_{uuid} {}

  void DoDurability() override { /* COLD marker persisted during Suspend_ (before this action runs) */ }

  bool ShouldReplicateInCommunity() const override { return false; }

  bool DoReplication(replication::ReplicationClient &client, const utils::UUID &main_uuid,
                     memgraph::system::Transaction const &txn) const override {
    auto check_response = [](const storage::replication::SuspendDatabaseRes &response) {
      return response.result != storage::replication::SuspendDatabaseRes::Result::FAILURE;
    };

    return client.StreamAndFinalizeDelta<storage::replication::SuspendDatabaseRpc>(
        check_response, main_uuid, txn.last_committed_system_timestamp(), txn.timestamp(), uuid_);
  }

  void PostReplication(replication::RoleMainData & /*mainData*/) const override {}

 private:
  utils::UUID uuid_;
};

// Hot/cold RESUME as a system action. See SuspendDatabase above. DoReplication streams a
// ResumeDatabaseRpc; the replica rebuilds its own copy (COLD -> HOT) from its on-disk artifacts
// identified by UUID, in system-timestamp order.
struct ResumeDatabase : memgraph::system::ISystemAction {
  ResumeDatabase(utils::UUID uuid, DatabaseAccess db_acc) : uuid_{uuid}, db_acc_{std::move(db_acc)} {}

  void DoDurability() override { /* COLD marker cleared during Resume_ (before this action runs) */ }

  bool ShouldReplicateInCommunity() const override { return false; }

  bool DoReplication(replication::ReplicationClient &client, const utils::UUID &main_uuid,
                     memgraph::system::Transaction const &txn) const override {
    auto check_response = [](const storage::replication::ResumeDatabaseRes &response) {
      return response.result != storage::replication::ResumeDatabaseRes::Result::FAILURE;
    };

    return client.StreamAndFinalizeDelta<storage::replication::ResumeDatabaseRpc>(
        check_response, main_uuid, txn.last_committed_system_timestamp(), txn.timestamp(), uuid_);
  }

  void PostReplication(replication::RoleMainData &mainData) const override {
    dbms::DbmsHandler::RecoverStorageReplication(db_acc_, mainData);
  }

 private:
  utils::UUID uuid_;
  DatabaseAccess db_acc_;
};

DbmsHandler::SuspendResult DbmsHandler::Suspend_(std::string_view name, system::Transaction *txn, bool for_recovery) {
  if (name == kDefaultDB) return std::unexpected{SuspendError::DEFAULT_DB};

  // Wall-clock start for the suspend-latency histogram; observed only on the success path below so a
  // rejected/aborted attempt (e.g. an ACTIVE_CONNECTIONS timeout) does not pollute the distribution.
  const auto suspend_start = std::chrono::steady_clock::now();

  SuspendedEntry entry;
  utils::Gatekeeper<Database> *gk = nullptr;
  std::optional<DatabaseAccess> acc;

  // PHASE A-pre (OFF-lock): stop the per-database features that keep the tenant pinned HOT.
  // Each Kafka/Pulsar stream consumer captures a shared_ptr<Interpreter> that owns a DatabaseAccess, so
  // the freeze in Phase A could never reach sole-accessor (count==1) while any stream exists -> suspend
  // would always fail ACTIVE_CONNECTIONS. on_suspend_ runs Streams::Shutdown() which stops the consumers
  // and releases those accessors WITHOUT deleting the durable stream metadata (resume rebuilds it).
  //
  // This MUST run with lock_ NOT held: Streams::Shutdown() performs an unbounded blocking join() of the
  // consumer threads, and joining while we hold lock_ would stall ALL db_handler_ access for however long
  // that join takes (and on a replica's single-threaded replication-apply thread it would stall
  // replication entirely). We take a brief shared lock_ only to mint the accessor, then release it before
  // joining.
  // Tracks whether on_suspend_ ran (not specifically "streams" — the callback's scope may grow beyond
  // stopping streams in the future).
  bool on_suspend_run = false;
  {
    std::optional<DatabaseAccess> sacc;
    {
      auto rd = std::shared_lock{lock_};
      auto a = db_handler_.Get(name);
      if (!a) return std::unexpected{SuspendError::NON_EXISTENT};
      sacc = std::move(*a);
    }  // release lock_ BEFORE joining consumer threads
    if (on_suspend_) {
      on_suspend_(*sacc);
      on_suspend_run = true;
    }
  }  // sacc released -> its count contribution is gone before Phase A mints the freeze accessor
  // UNDO guard: a failed/aborted suspend must NOT silently leave the streams stopped. Fires on every path
  // that does not commit the suspend (early return below, or a post-freeze throw after the abort_suspend
  // rollback restores HOT). Declared BEFORE the abort_suspend rollback so it fires AFTER it (LIFO) -> the
  // tenant is HOT again by the time we restore streams. Disabled once finish_suspend() commits the COLD.
  auto stream_restore = utils::OnScopeExit{[&] {
    if (!on_suspend_run || !restore_streams_) return;
    auto rd = std::shared_lock{lock_};
    if (auto a = db_handler_.Get(name)) restore_streams_(*a);
  }};
  {
    // PHASE A — eligibility check + metadata capture + freeze, all under the shared lock_.
    //
    // try_begin_suspend() is called INSIDE this shared_lock scope (not after).
    // Rationale: between releasing the shared lock and try_begin_suspend(), a concurrent
    // DROP DATABASE (Delete -> DeferDelete) sees state HOT, moves `gk` out of the map, and
    // erases it -> gk dangling -> heap-UAF on try_begin_suspend(). By calling
    // try_begin_suspend() under the shared lock, the state transitions to SUSPENDING while
    // lock_ is held; a concurrent Drop then acquires the exclusive lock_ (mutually exclusive
    // with our shared lock), sees SUSPENDING, and returns DeleteError::USING without touching
    // `gk` — so `gk` remains valid through the lock-free Phase B/C below.
    //
    // NOTE: try_begin_suspend() waits up to 100ms for count==1. Holding the shared lock_
    // during that wait is acceptable: suspend is rare/background; accessor release and
    // access() use the gatekeeper's own mutex, not lock_, so count can still drain; only
    // exclusive-lock writers (Drop/New) briefly wait on lock_.
    //
    // CONCURRENCY ASSUMPTION: at this primitive level, two concurrent Suspend_ calls for the SAME
    // tenant would each mint their own accessor above, so each sees the other's accessor and both fail
    // try_begin_suspend()'s ACTIVE_CONNECTIONS check — single-winner exclusivity is NOT enforced by this
    // primitive itself. It relies on DDL being serialized upstream by the system transaction. A future
    // caller that bypasses that serialization (e.g. an asynchronous/direct resume-triggered suspend) must
    // add its own per-tenant exclusion before reaching this point.
    auto rd = std::shared_lock{lock_};
    auto a = db_handler_.Get(name);  // nullopt if absent OR not HOT (already cold)
    if (!a) return std::unexpected{SuspendError::NON_EXISTENT};
    auto *db = a->get();
    auto *st = db->storage();
    // Suspend requires {periodic snapshot + WAL} durability AND a RUNTIME storage mode of
    // IN_MEMORY_TRANSACTIONAL. The runtime-mode check is load-bearing and must NOT be replaced by
    // the config: a tenant switched to IN_MEMORY_ANALYTICAL at runtime suppresses WAL
    // (InitializeWalFile() returns false for analytical) and pauses the snapshot runner, yet
    // IsDurabilityCompleteForSuspend() only inspects the creation-time config.
    // Rejecting anything not in-memory transactional covers both ON_DISK_TRANSACTIONAL and
    // IN_MEMORY_ANALYTICAL.
    if (st->GetStorageMode() != storage::StorageMode::IN_MEMORY_TRANSACTIONAL) {
      return std::unexpected{SuspendError::NOT_IN_MEMORY};
    }
    if (!st->IsDurabilityCompleteForSuspend()) {
      if (!for_recovery) return std::unexpected{SuspendError::DURABILITY_INCOMPLETE};
      // In recovery the replica converges to MAIN's AUTHORITATIVE cold set. The durability-complete
      // gate protects a USER-initiated SUSPEND from creating an unrecoverable cold tenant; in recovery
      // the tenant is already COLD on MAIN, so we bypass the gate and suspend here regardless. Suspend
      // itself no longer writes a snapshot, so recoverability of the resulting local cold shell rests
      // entirely on THIS replica's own periodic-snapshot+WAL durability state, not on anything suspend
      // does — if that state is incomplete, this cold shell may not be locally recoverable (no
      // snapshot, no/partial WAL). The cluster stays consistent regardless via the higher-level
      // SystemRecoveryHandler / replica resync from MAIN, not via a suspend-time snapshot.
      // Reachable only when this replica's durability differs from MAIN's (e.g. the WAL/snapshot config
      // is inconsistent across the cluster) — bypass with a loud warning rather than failing recovery
      // and stalling the replica in a BEHIND retry loop.
      spdlog::warn(
          "Force-suspending database {} during recovery although its local durability is incomplete "
          "(periodic snapshot + WAL not enabled here). MAIN is authoritative for the cold set, so this "
          "replica converges regardless, but the resulting local cold shell may not be locally "
          "recoverable (no snapshot, no/partial WAL) — this usually means the durability config differs "
          "from MAIN — align the WAL/snapshot durability flags across the cluster.",
          name);
    }
    // Suspend does NOT gate on whether replicas are registered. Hot/cold is a system-replicated operation
    // (sibling of CREATE/DROP DATABASE): suspending a tenant that has replicas is the SUPPORTED flow — the
    // SuspendDatabase system action streams a SuspendDatabaseRpc so each replica tears down its own
    // copy in system-timestamp order. The earlier "reject while replicating" guard encoded only the
    // v1 node-local limitation (no wire to tell replicas) and protects no MAIN-local invariant:
    // IsDurabilityCompleteForSuspend() + the freeze-to-count==1 below already guarantee MAIN's state
    // is fully durable before teardown. On a replica this predicate is false anyway (replication
    // storage clients live on MAIN), so the path is identical there.

    entry.salient = db->config().salient;
    entry.rel_dir = std::filesystem::relative(db->config().durability.storage_directory,
                                              default_config_.durability.storage_directory);

    gk = db_handler_.GetGatekeeper(name);  // stable pointer to the in-map gatekeeper
    acc = std::move(*a);                   // hold the accessor across phases (count includes it)

    // Transition HOT -> SUSPENDING under the shared lock (LAST step in Phase A).
    if (!gk->try_begin_suspend()) return std::unexpected{SuspendError::ACTIVE_CONNECTIONS};
  }
  // State is now SUSPENDING. Concurrent Drop sees SUSPENDING and returns USING without
  // erasing gk, so gk is valid for the rest of this function (lock-free Phase B/C).

  // RAII rollback guard. If anything in the SUSPENDING window throws, abort_suspend() restores HOT
  // so the gatekeeper is not permanently stuck SUSPENDING (which would hang ~Gatekeeper).
  auto rollback = utils::OnScopeExit{[&] { gk->abort_suspend(); }};

  // PHASE B — post-freeze work (lock-free; gk is SUSPENDING so Drop is rejected).
  // No post-freeze replica-registration re-check either — see Phase A rationale above. A
  // RegisterReplica racing into the A->freeze window only adds replicas, which is now allowed.

  // Suspend takes NO proactive/consolidating snapshot. Durability at suspend is whatever the
  // periodic snapshot + WAL already provide (guaranteed complete by IsDurabilityCompleteForSuspend()
  // above), plus — if --storage-snapshot-on-exit is enabled — the snapshot InMemoryStorage's own
  // destructor writes during its normal teardown below (finish_suspend() -> ~InMemoryStorage).
  // Resume recovers via recover_on_startup (snapshot-if-any + WAL replay from the tenant's own
  // durability directory); it does not depend on a suspend-time snapshot existing.

  // Capture the last-hot stats snapshot POST-freeze (count==1, no concurrent commit can advance it
  // between this read and teardown). Served by cold SHOW STORAGE INFO / SHOW DATABASES. Use GetInfo()
  // (not GetBaseInfo()): the latter only fills vertex/edge/memory/disk counters and leaves
  // storage_mode, isolation_level, index/constraint counts, and the durability/compression flags
  // value-initialized, so cold SHOW STORAGE INFO would report wrong values for those fields.
  // GetInfo() layers them on top of GetBaseInfo() and is deadlock-safe here: it only takes storage's
  // main_lock_ shared (via Access(READ)), which is free in this frozen SUSPENDING window. `st`'s
  // concrete type is InMemoryStorage (guaranteed by the IN_MEMORY_TRANSACTIONAL check earlier), so
  // GetInfo() dispatches correctly.
  {
    auto *st = (*acc)->storage();
    entry.cold_stats = st->GetInfo();
  }

  // PHASE C — record metadata (lock_), then heavy teardown OUTSIDE lock_.
  const auto tenant_uuid = entry.salient.uuid;
  std::string cold_key;
  std::string cold_val;
  if (durability_) {
    cold_key = Durability::GenKey(name);
    cold_val = Durability::GenColdVal(entry.salient.uuid, entry.rel_dir, entry.cold_stats);
  }
  // Pre-allocate the suspended_ map node BEFORE committing the durable COLD marker so no allocation can
  // throw after durability_->Put succeeds.
  std::map<std::string, SuspendedEntry, std::less<>> staged_suspended;
  staged_suspended.emplace(std::string{name}, std::move(entry));
  auto staged_node = staged_suspended.extract(staged_suspended.begin());

  auto *db_for_teardown = acc->get();
  acc.reset();  // drop our accessor -> count == 0 (state still SUSPENDING)
  {
    auto wr = std::lock_guard{lock_};
    if (durability_) {
      bool marker_persisted = false;
      try {
        marker_persisted = durability_->Put(cold_key, cold_val);
      } catch (const std::exception &e) {
        spdlog::error("hot/cold suspend: failed to persist cold durability marker for '{}': {}", name, e.what());
        marker_persisted = false;
      } catch (...) {
        spdlog::error("hot/cold suspend: failed to persist cold durability marker for '{}'.", name);
        marker_persisted = false;
      }
      if (!marker_persisted) {
        spdlog::error("hot/cold suspend: aborting suspend of '{}' because the cold marker was not persisted.", name);
        return std::unexpected{SuspendError::DURABILITY_FAILED};
      }
    }
    if (auto existing = suspended_.find(name); existing != suspended_.end()) {
      suspended_.erase(existing);
    }
    suspended_.insert(std::move(staged_node));
    metrics::Metrics().global.database_suspends->Increment();
    UpdateColdGauge_();
  }

  // Disable the rollback guard BEFORE calling finish_suspend(). finish_suspend() transitions
  // SUSPENDING -> COLD; if the guard fired afterwards it would abort_suspend() on a non-SUSPENDING
  // state and trip the debug assert.
  rollback.Disable();

  // Stop this tenant's background tasks (stream consumers, after-commit trigger pool, storage's TTL
  // scheduler + async indexer) HERE — while the gatekeeper mutex is NOT held. finish_suspend() below
  // takes that mutex and holds it across ~Database -> ~InMemoryStorage, whose StopAllBackgroundTasks()
  // JOINS the TTL / async-indexer threads. Those threads call make_database_protector() ->
  // Gatekeeper::Ref::access(), which blocks acquiring the very mutex finish_suspend()
  // holds -> the join would deadlock (joiner holds the lock the joinee waits on). By stopping them now,
  // that in-destructor join is a no-op. The WAL-finalize-under-mutex suspend->resume directory handoff
  // that finish_suspend() protects is unaffected: only the thread joins move earlier; FinalizeWal still
  // runs under the mutex inside finish_suspend(). Placed AFTER rollback.Disable() (the point of no
  // return) so a rolled-back suspend can never leave a HOT tenant with dead background schedulers.
  db_for_teardown->StopAllBackgroundTasks();

  // OUTSIDE lock_: value_.reset() -> ~Database (stop threads, FinalizeWal, NO exit snapshot).
  // The COLD shell stays in db_handler_ so a later resume can move-assign a fresh gatekeeper.
  gk->finish_suspend();

  // The database teardown frees its own arena, but many allocations remain in shared jemalloc arenas.
  // Force a global purge so SUSPEND returns only after the tenant's memory has been reclaimed.
  if (!memory::PurgeUnusedMemory()) {
    spdlog::warn("Failed to purge unused allocator memory after suspending database.");
  }

  // The suspend is committed (tenant is COLD, in-memory storage and stream consumers dropped;
  // durable stream metadata persists for resume). Keep the streams stopped — do NOT run the undo guard.
  // finish_suspend()'s only fallible step is ~Database (noexcept), so no throw can unwind into this gap.
  stream_restore.Disable();

  // Record the system action so the suspend is ordered + replicated like CREATE/DROP DATABASE.
  // Done only after the local teardown commits; the replica wire is filled by DoReplication.
  if (txn) txn->AddAction<SuspendDatabase>(tenant_uuid);

  // Successful suspend: record end-to-end latency (includes teardown).
  metrics::Metrics().global.database_suspend_latency_seconds->Observe(
      std::chrono::duration<double>(std::chrono::steady_clock::now() - suspend_start).count());

  spdlog::info("hot/cold: database '{}' suspended (HOT -> COLD)", name);
  return {};
}

DbmsHandler::ResumeResult DbmsHandler::Resume_(std::string_view name, system::Transaction *txn, bool *already_hot) {
  // Outer loop: a loser keeps `continue`-ing only while the winner is demonstrably alive
  // (RESUMING/SUSPENDING), re-initializing gk/salient/rel_dir/won_resume from the map under a fresh
  // shared lock on each pass. All other exits (NON_EXISTENT, already-HOT, timeout, build failure, and
  // a CONFIRMED-COLD winner abort) are `return`s — a loser that confirms the winner aborted fails fast
  // with RECOVERY_FAILED rather than auto-retrying; RESUME is idempotent/retriable, so the caller
  // re-issues.
  // Wall-clock start for the resume-latency histogram; observed only on the winner's successful publish
  // below (the path that bumps the resume counter), so loser/error returns do not skew the distribution.
  const auto resume_start = std::chrono::steady_clock::now();
  // Set once this call has looped waiting on a concurrent winner. A HOT observation on a re-entered pass
  // is then a real COLD->HOT transition we waited through, NOT an idempotent already-HOT no-op.
  bool waited_through_transition = false;
  while (true) {
    utils::Gatekeeper<Database> *gk = nullptr;
    // Copy only the two fields the off-lock build needs (salient + rel_dir) instead of the full
    // SuspendedEntry (which also holds cold_stats).
    storage::SalientConfig salient;
    std::filesystem::path rel_dir;
    bool won_resume = false;
    {
      // PHASE A — decide + acquire the single-flight token, all under the shared lock_.
      //
      // begin_resume() is called INSIDE this shared-lock scope (not after): between releasing the
      // lock and begin_resume(), a concurrent DROP of a COLD tenant (under exclusive lock_) could
      // erase the COLD shell, leaving `gk` dangling -> begin_resume() would be a UAF. By transitioning
      // COLD -> RESUMING while lock_ is held, a concurrent DROP then takes the exclusive lock and calls
      // DeleteCold_ which checks gk->state() — if not strictly COLD (it is RESUMING) DeleteCold_
      // returns USING and refuses to erase — so `gk` stays valid through the slow off-lock build/publish.
      // This invariant is ENFORCED inside DeleteCold_/EraseColdShell (not merely claimed): both functions
      // check state() under the caller's exclusive lock_ before touching items_ or suspended_.
      auto rd = std::shared_lock{lock_};
      if (auto a = db_handler_.Get(name)) {
        // Already HOT — share its accessor. already_hot is true only on the FIRST pass (the tenant was
        // HOT before this call did anything: idempotent no-op). On a re-entered pass we got here by
        // waiting through a concurrent winner's COLD -> HOT publish, so report a real resume.
        if (already_hot) *already_hot = !waited_through_transition;
        return std::move(*a);
      }
      gk = db_handler_.GetGatekeeper(name);  // stable pointer to the in-map gatekeeper
      if (!gk) return std::unexpected{ResumeError::NON_EXISTENT};
      auto it = suspended_.find(name);
      if (it == suspended_.end()) return std::unexpected{ResumeError::NON_EXISTENT};
      salient = it->second.salient;  // copy only the two fields the off-lock build needs
      rel_dir = it->second.rel_dir;
      won_resume = gk->begin_resume();  // COLD -> RESUMING (LAST step in Phase A)
    }

    // SINGLE-FLIGHT — loser path: someone else holds RESUMING. Poll Get(name) until HOT.
    // The deadline is re-armed on every iteration that confirms the winner is still RESUMING
    // (demonstrably alive and building), so a healthy large-tenant rebuild that takes longer than the
    // liveness window does NOT spuriously fail. kMaxWait is an absolute ceiling that still fires if the
    // winner is stuck-alive (holds RESUMING but never completes). We poll by NAME (not the gatekeeper
    // cv_): the winner publishes via `*gk = std::move(fresh)`, which destroys the old GKInternals (and
    // its cv_), so waiting on that cv_ would be a use-after-free.
    //
    // KNOWN LIMITATION: liveness is inferred purely from the RESUMING state, not an independent probe of
    // the winner thread. A winner that is alive but HUNG in recovery I/O (e.g. a stuck disk) keeps every
    // loser re-arming its deadline and waiting up to resume_retry_policy_.max_wait, repeatedly, forever.
    // This is an accepted blocking-I/O limitation (same class as finish_suspend()'s unbounded teardown
    // join, see the comment above Suspend_'s on_suspend_ call) — a stuck-RESUMING observability metric is
    // a planned follow-up, not implemented here.
    if (!won_resume) {
      constexpr auto kPollStep = std::chrono::milliseconds(5);
      // How long to wait after the LAST confirmed-RESUMING observation before concluding the winner
      // is dead. Large enough for a healthy rebuild under IO pressure, small enough to surface a
      // crashed winner before a client request timeout.
      // Absolute ceiling from poll entry: bounds a stuck-alive winner (RESUMING forever).
      const auto absolute_ceiling = std::chrono::steady_clock::now() + resume_retry_policy_.max_wait;
      auto deadline = std::chrono::steady_clock::now() + resume_retry_policy_.winner_liveness_window;
      while (std::chrono::steady_clock::now() < deadline && std::chrono::steady_clock::now() < absolute_ceiling) {
        bool retry_after_cold_fallback = false;
        {
          auto rd = std::shared_lock{lock_};
          if (auto a = db_handler_.Get(name)) {
            // Winner published HOT — this loser observed a real COLD -> HOT transition, not an
            // already-hot no-op, so already_hot stays false.
            if (already_hot) *already_hot = false;
            return std::move(*a);
          }
          // Re-fetch the gatekeeper by name under THIS shared lock rather than dereferencing the raw
          // `gk` captured in Phase A: a concurrent DROP of the (now-COLD) tenant during a sleep window
          // can free the GKInternals `gk` points at. nullptr (dropped) or COLD => treat as a fallback
          // and break out of polling to the authoritative post-poll re-check below, which returns
          // NON_EXISTENT (dropped) or RECOVERY_FAILED (winner aborted). We must NOT re-check while
          // holding this shared lock (the winner path takes the exclusive lock_ on the same thread ->
          // self-deadlock on the PREFER_READER rwlock): set a flag, drop the lock, then break.
          auto *live_gk = db_handler_.GetGatekeeper(name);
          if (!live_gk || live_gk->state() == utils::GatekeeperState::COLD) {
            retry_after_cold_fallback = true;
          } else if (live_gk->state() == utils::GatekeeperState::RESUMING ||
                     live_gk->state() == utils::GatekeeperState::SUSPENDING) {
            // Winner is demonstrably still alive — extend the liveness deadline. SUSPENDING is a live,
            // bounded wait-state too (not a COLD-fallback): Suspend_ inserts into `suspended_` (Phase B,
            // before finish_suspend()) BEFORE the SUSPENDING -> COLD flip, so by the time any observer
            // sees COLD the `suspended_` entry is guaranteed present. Treating SUSPENDING as a fallback
            // here would restart Phase A into the freeze-visible-but-not-yet-in-`suspended_` window,
            // which would spuriously return NON_EXISTENT for a tenant that in fact exists (racing a
            // concurrent SUSPEND against this RESUME).
            deadline = std::chrono::steady_clock::now() + resume_retry_policy_.winner_liveness_window;
          }
        }
        if (retry_after_cold_fallback) break;  // exit poll loop; fall through to the post-poll decision
        std::this_thread::sleep_for(kPollStep);
      }
      bool winner_still_resuming = false;
      {
        auto rd = std::shared_lock{lock_};
        auto *live_gk = db_handler_.GetGatekeeper(name);
        if (!live_gk) return std::unexpected{ResumeError::NON_EXISTENT};  // dropped mid-resume
        // HOT: the winner just published — share its accessor. Real COLD -> HOT transition, so
        // already_hot stays false (same reasoning as the in-poll HOT check above).
        if (auto a = db_handler_.Get(name)) {
          if (already_hot) *already_hot = false;
          return std::move(*a);
        }
        // RESUMING: a concurrent winner is still actively rebuilding. SUSPENDING: a DIFFERENT tenant
        // transition is live (see the loser-poll SUSPENDING note above) — its `suspended_` entry is
        // guaranteed present once it reaches COLD, so this is also just "still alive, keep waiting", not
        // a fallback. A demonstrably-live winner must NEVER be turned into a RECOVERY_FAILED, however
        // long its rebuild takes (a huge tenant replaying a long WAL is legitimately slow) — only restart
        // Phase A to keep waiting; a CONFIRMED-COLD (aborted) winner is the only case that fails fast.
        winner_still_resuming = (live_gk->state() == utils::GatekeeperState::RESUMING ||
                                 live_gk->state() == utils::GatekeeperState::SUSPENDING);
        // COLD / any other non-terminal state: the winner aborted (or a SUSPEND completed) — fail fast
        // below with RECOVERY_FAILED instead of auto-retrying.
      }
      // A live (RESUMING/SUSPENDING) winner: keep waiting, bounded by max_wait via the outer loop
      // re-arming the poll (this is the HC-1 fix: the absolute_ceiling can fire purely because the
      // winner stayed RESUMING, and that must not be treated as an abort).
      if (winner_still_resuming) {
        waited_through_transition = true;  // a HOT observation on re-entry is now a real resume, not already-HOT
        continue;
      }
      // Winner aborted -> tenant went COLD: the resume we were waiting on failed. Fail fast — RESUME is
      // idempotent and retriable, so the caller re-issues rather than us auto-retrying in-call (which was
      // a livelock risk under concurrent failing resumes, and needed a bounded counter). suspended_ is
      // untouched by abort_resume(), so the tenant stays retriable.
      return std::unexpected{ResumeError::RECOVERY_FAILED};
    }

    // WE are the winner (state RESUMING). Build OFF the map and recover (SLOW, NO lock_ held).
    // `fresh` lives in this scope so that, on the throw path, the catch can release `acc` BEFORE
    // `fresh` destructs: ~Gatekeeper waits for count == 0, so an outstanding `acc` would self-deadlock
    // the fresh gatekeeper's destruction.
    try {
      if (!IsExpectedTenantRelDir_(name, salient.uuid, rel_dir)) {
        spdlog::error("hot/cold resume: unexpected relative directory '{}' for database '{}' (UUID '{}').",
                      rel_dir.string(),
                      name,
                      std::string{salient.uuid});
        if (gk->state() == utils::GatekeeperState::RESUMING) gk->abort_resume();
        return std::unexpected{ResumeError::RECOVERY_FAILED};
      }
      std::optional<int64_t> resolved_limit;
      if (tenant_profiles_) {
        const auto limit_lookup = tenant_profiles_->ResolveDatabaseLimit(name);
        if (!limit_lookup.has_value()) {
          spdlog::error(
              "hot/cold resume: refusing to resume database '{}' because its tenant profile limit could not be read.",
              name);
          if (gk->state() == utils::GatekeeperState::RESUMING) gk->abort_resume();
          return std::unexpected{ResumeError::RECOVERY_FAILED};
        }
        resolved_limit = *limit_lookup;
      }

      storage::Config cfg = default_config_;
      cfg.salient = salient;
      cfg.tenant_memory_limit = resolved_limit;
      cfg.durability.recover_on_startup = true;
      storage::UpdatePaths(cfg, default_config_.durability.storage_directory / rel_dir);
      auto fresh = db_handler_.BuildDetached(std::move(cfg));  // Database ctor recovers from disk with limit applied
      DatabaseAccess acc = fresh.access().value();             // fresh gk is HOT => has_value
      auto rollback_to_cold = [&] {
        acc.reset();
        {
          auto dying = std::move(fresh);
        }
        gk->abort_resume();
      };
      try {
        std::string hot_key;
        std::string hot_val;
        if (durability_) {
          hot_key = Durability::GenKey(name);
          hot_val = Durability::GenVal(salient.uuid, rel_dir);
        }
        if (on_resume_) on_resume_(acc);
        if (ttl_user_check_) acc->storage()->ttl_.SetUserCheck(ttl_user_check_);
        {
          auto wr = std::unique_lock{lock_};
          if (durability_ && !durability_->Put(hot_key, hot_val)) {
            wr.unlock();
            spdlog::error("hot/cold resume: failed to persist hot durability marker for '{}'; rolling back to COLD.",
                          name);
            rollback_to_cold();
            return std::unexpected{ResumeError::RECOVERY_FAILED};
          }
          auto it = suspended_.find(name);
          *gk = std::move(fresh);
          if (it != suspended_.end()) suspended_.erase(it);
          metrics::Metrics().global.database_resumes->Increment();
          UpdateColdGauge_();
        }
      } catch (const std::exception &e) {
        spdlog::warn(
            "hot/cold resume: the pre-publish resume arm for '{}' threw ({}); rolling back to COLD (retriable).",
            name,
            e.what());
        rollback_to_cold();
        return std::unexpected{ResumeError::RECOVERY_FAILED};
      } catch (...) {
        rollback_to_cold();
        return std::unexpected{ResumeError::RECOVERY_FAILED};
      }

      auto best_effort = [&](std::string_view what, auto &&fn) {
        try {
          fn();
        } catch (const std::exception &e) {
          spdlog::warn("hot/cold resume: {} for '{}' threw ({}); database is live (HOT).", what, name, e.what());
        } catch (...) {
          spdlog::warn("hot/cold resume: {} for '{}' threw; database is live (HOT).", what, name);
        }
      };

      best_effort("recording resume latency", [&] {
        metrics::Metrics().global.database_resume_latency_seconds->Observe(
            std::chrono::duration<double>(std::chrono::steady_clock::now() - resume_start).count());
      });

      best_effort("recording the resume system action (may not replicate until the next system sync)", [&] {
        if (txn) txn->AddAction<ResumeDatabase>(salient.uuid, acc);
        spdlog::info("hot/cold: database '{}' resumed (COLD -> HOT)", name);
      });
      if (already_hot) *already_hot = false;
      return acc;
    } catch (const std::exception &e) {
      // Recovery (BuildDetached) threw before `acc`/`fresh` existed. No live accessor to release.
      // Guarded by state(): reachable today only from a pre-publish BuildDetached throw, where state is
      // still RESUMING (behavior unchanged). The guard is defense-in-depth against a hypothetical future
      // post-publish escape reaching this outer catch — calling abort_resume() on an already-HOT
      // gatekeeper would (under NDEBUG) silently force it back to COLD.
      spdlog::warn("hot/cold resume: recovering database '{}' failed ({}); staying COLD (retriable).", name, e.what());
      // RESUMING -> COLD; suspended_ untouched => retriable.
      if (gk->state() == utils::GatekeeperState::RESUMING) gk->abort_resume();
      return std::unexpected{ResumeError::RECOVERY_FAILED};
    } catch (...) {
      // Same as above (non-std throw): no exception text to log.
      // RESUMING -> COLD; suspended_ untouched => retriable.
      if (gk->state() == utils::GatekeeperState::RESUMING) gk->abort_resume();
      return std::unexpected{ResumeError::RECOVERY_FAILED};
    }
  }  // end outer while(true) — all winner exits are `return`, so falling out is unreachable
}

DbmsHandler::SuspendResult DbmsHandler::SuspendByUUID(utils::UUID uuid, system::Transaction *txn) {
  // Resolve UUID -> name from the HOT set, then drop the lock before delegating to Suspend_ (which
  // re-acquires lock_ and freezes the tenant; holding an accessor here would keep count > 1 and make
  // try_begin_suspend() fail). A COLD shell has no accessor, so it is correctly not matched — a
  // SuspendDatabaseRpc for an already-COLD tenant resolves to NON_EXISTENT and the handler treats it
  // as NO_NEED (idempotent re-apply).
  std::string name;
  {
    auto rd = std::shared_lock{lock_};
    auto it = FindHotByUuid_(uuid);  // access() is nullopt for a non-HOT (COLD) shell => skipped
    if (it == db_handler_.end()) return std::unexpected{SuspendError::NON_EXISTENT};
    name = it->first;
  }
  return Suspend_(name, txn);
}

DbmsHandler::ResumeResult DbmsHandler::ResumeByUUID(utils::UUID uuid, system::Transaction *txn) {
  std::string name;
  {
    auto rd = std::shared_lock{lock_};
    auto it = FindSuspendedByUuid_(uuid);
    if (it == suspended_.end()) return std::unexpected{ResumeError::NON_EXISTENT};
    name = it->first;
  }
  return Resume_(name, txn);
}

void DbmsHandler::ApplyColdRecoveryMeta(std::string_view name, const storage::ColdTenantRecovery &meta) {
  auto wr = std::lock_guard{lock_};
  auto it = suspended_.find(name);
  if (it == suspended_.end()) return;
  if (it->second.cold_stats == meta.stats && !it->second.stats_dirty) return;
  it->second.cold_stats = meta.stats;

  if (durability_) {
    const auto &entry = it->second;
    if (durability_->Put(Durability::GenKey(name),
                         Durability::GenColdVal(entry.salient.uuid, entry.rel_dir, entry.cold_stats))) {
      it->second.stats_dirty = false;
    } else {
      it->second.stats_dirty = true;
      spdlog::warn("hot/cold recovery: failed to persist cold stats for database '{}'; marked dirty for retry.", name);
    }
  }
}

#endif  // MG_ENTERPRISE

nlohmann::json ToJson(const Statistics &stats) {
  nlohmann::json res;

  res["edges"] = stats.num_edges;
  res["vertices"] = stats.num_vertex;
  res["triggers"] = stats.triggers;
  res["streams"] = stats.streams;
  res["users"] = stats.users;
  res["roles"] = stats.roles;
  res["databases"] = stats.num_databases;
  res["indices"] = stats.indices;
  res["constraints"] = stats.constraints;
  res["storage_modes"] = {{storage::StorageModeToString(static_cast<storage::StorageMode>(0)), stats.storage_modes[0]},
                          {storage::StorageModeToString(static_cast<storage::StorageMode>(1)), stats.storage_modes[1]},
                          {storage::StorageModeToString(static_cast<storage::StorageMode>(2)), stats.storage_modes[2]}};
  res["isolation_levels"] = {
      {storage::IsolationLevelToString(static_cast<storage::IsolationLevel>(0)), stats.isolation_levels[0]},
      {storage::IsolationLevelToString(static_cast<storage::IsolationLevel>(1)), stats.isolation_levels[1]},
      {storage::IsolationLevelToString(static_cast<storage::IsolationLevel>(2)), stats.isolation_levels[2]}};
  res["durability"] = {{"snapshot_enabled", stats.snapshot_enabled}, {"WAL_enabled", stats.wal_enabled}};
  res["property_store_compression_enabled"] = stats.property_store_compression_enabled;
  res["property_store_compression_level"] = {
      {utils::CompressionLevelToString(utils::CompressionLevel::LOW), stats.property_store_compression_level[0]},
      {utils::CompressionLevelToString(utils::CompressionLevel::MID), stats.property_store_compression_level[1]},
      {utils::CompressionLevelToString(utils::CompressionLevel::HIGH), stats.property_store_compression_level[2]}};
  res["label_node_count_histogram"] = {{"1-9", stats.label_node_count_histogram[0]},
                                       {"10-99", stats.label_node_count_histogram[1]},
                                       {"100-999", stats.label_node_count_histogram[2]},
                                       {"1K-9.99K", stats.label_node_count_histogram[3]},
                                       {"10K-99.9K", stats.label_node_count_histogram[4]},
                                       {"100K-999K", stats.label_node_count_histogram[5]},
                                       {"1M+", stats.label_node_count_histogram[6]}};
  res["num_parameters"] = stats.num_parameters;
  res["num_descriptions"] = stats.num_descriptions;

  return res;
}

}  // namespace memgraph::dbms
