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

#include "dbms/database.hpp"

#include <functional>
#include <memory>

#include "spdlog/spdlog.h"

#include "dbms/database_info.hpp"
#include "dbms/database_protector.hpp"
#include "dbms/inmemory/replication_handlers.hpp"
#include "dbms/inmemory/storage_helper.hpp"
#include "flags/coord_flag_env_handler.hpp"
#include "flags/general.hpp"
#include "memory/db_arena.hpp"
#include "metrics/prometheus_metrics.hpp"
#include "query/stream/streams.hpp"
#include "query/trigger.hpp"
#include "storage/v2/database_protector.hpp"
#include "storage/v2/disk/storage.hpp"
#include "storage/v2/storage.hpp"
#include "storage/v2/storage_mode.hpp"
#include "storage/v2/ttl.hpp"

template struct memgraph::utils::Gatekeeper<memgraph::dbms::Database>;

namespace memgraph::dbms {

struct PlanInvalidatorForDatabase : storage::PlanInvalidator {
  explicit PlanInvalidatorForDatabase(query::PlanCacheLRU &planCache) : plan_cache(planCache) {}

  auto invalidate_for_timestamp_wrapper(std::function<bool(uint64_t)> func) -> std::function<bool(uint64_t)> override {
    return [&plan_cache = plan_cache, func = std::move(func)](uint64_t timestamp) {
      return plan_cache.WithLock([&](query::PlanCache_t &cache) {
        auto do_reset = func(timestamp);
        if (do_reset) {
          cache.reset();
        }
        return do_reset;
      });
    };
  }

  bool invalidate_now(std::function<bool()> func) override {
    return plan_cache.WithLock([&](query::PlanCache_t &cache) {
      auto do_reset = func();
      if (do_reset) {
        cache.reset();
      }
      return do_reset;
    });
  }

 private:
  // Storage and Plan cache exist in Database
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  query::PlanCacheLRU &plan_cache;
};

Database::~Database() {
  // Every teardown path (drop, suspend, drop-recreate, process exit) destroys via this destructor, and
  // the cached 2PC accessor is storage-level, not gatekeeper-counted -- this is the one choke point
  // that reaches it. Safe: the body runs before storage_ is destroyed.
  //
  // Must not call any utils::Gatekeeper method here -- finish_suspend() holds GKInternals::mutex_
  // across ~Database (gatekeeper.hpp:394), so re-entry would self-deadlock.
  try {
    // GatekeeperGuard clears arena TLS around Database destruction; re-establish it so Abort()'s
    // delta walk allocates against this tenant's arena, not stale TLS.
    const memory::DbArenaScope db_arena_scope{this};
    InMemoryReplicationHandlers::AbortTwoPCForTenant(uuid());
  } catch (const std::exception &e) {
    // spdlog can throw; swallow it so it can't escape this implicitly-noexcept destructor (mirrors ~Gatekeeper).
    try {
      spdlog::error(
          "Database::~Database: failed to abort cached 2PC for tenant {}: {} -- prepared transaction stays pinned in "
          "the commit log.",
          std::string{uuid()},
          e.what());
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
  } catch (...) {
    try {
      spdlog::error(
          "Database::~Database: failed to abort cached 2PC for tenant {} (unknown exception) -- prepared transaction "
          "stays pinned in the commit log.",
          std::string{uuid()});
    } catch (...) {  // NOLINT(bugprone-empty-catch)
    }
  }
}

std::unique_ptr<storage::Accessor> Database::Access(storage::StorageAccessType rw_type,
                                                    std::optional<storage::IsolationLevel> override_isolation_level,
                                                    std::optional<std::chrono::milliseconds> timeout) {
  return storage_->Access(rw_type, override_isolation_level, timeout);
}

std::unique_ptr<storage::Accessor> Database::UniqueAccess(
    std::optional<storage::IsolationLevel> override_isolation_level, std::optional<std::chrono::milliseconds> timeout) {
  return storage_->UniqueAccess(override_isolation_level, timeout);
}

std::unique_ptr<storage::Accessor> Database::ReadOnlyAccess(
    std::optional<storage::IsolationLevel> override_isolation_level, std::optional<std::chrono::milliseconds> timeout) {
  return storage_->ReadOnlyAccess(override_isolation_level, timeout);
}

std::string Database::name() const { return storage_->name(); }

utils::SafeString::ConstSafeWrapper Database::name_view() const { return storage_->name_view(); }

void Database::Rename(std::string_view new_name) {
  storage_->config_.salient.name = new_name;
  metrics_.Rename(new_name);
}

const utils::UUID &Database::uuid() const { return storage_->uuid(); }

const storage::Config &Database::config() const { return storage_->config_; }

storage::StorageMode Database::GetStorageMode() const noexcept { return storage_->GetStorageMode(); }

storage::ttl::TTL &Database::ttl() { return storage_->ttl_; }

memory::ArenaPool &Database::Arena() noexcept { return *db_arena_; }

memory::ArenaPool &Database::Arena() const noexcept { return *db_arena_; }

namespace {

// A coordinator reports no per-database metrics under OpenMetrics, and some databases ask not to be
// measured at all; both hold an empty registration, which releases nothing.
auto RegisterMetrics(storage::Config const &config) -> metrics::PrometheusMetrics::Registration {
  if (!config.register_metrics) return {};
  if (FLAGS_metrics_format == "OpenMetrics" && flags::CoordinationSetupInstance().IsCoordinator()) return {};
  return metrics::Metrics().AddDatabase(config.salient.uuid, config.salient.name.str());
}

}  // namespace

Database::Database(storage::Config config)
    : metrics_(RegisterMetrics(config)),
      db_arena_(std::make_unique<memory::ArenaPool>(&db_memory_tracker_)),
      after_commit_trigger_pool_{1,
                                 // After-commit triggers run on a dedicated DB worker.
                                 // Keep a DB arena scope alive for the full worker lifetime.
                                 [this]() -> utils::ThreadPool::TaskSignature {
                                   auto db_arena_scope = std::make_unique<memory::DbArenaScope>(this);
                                   return [db_arena_scope = std::move(db_arena_scope)]() mutable {
                                     db_arena_scope.reset();
                                   };
                                 }},
      streams_(
          std::make_unique<query::stream::Streams>(config.durability.storage_directory / "streams", db_arena_.get())),
      plan_cache_{FLAGS_query_plan_cache_max_size} {
  // Route all constructor-body allocations (storage init, recovery, index structures) to this DB's arena.
  const memory::DbArenaScope db_arena_scope{this};

  // Postpone creation after the scope has been created
  trigger_store_ = std::make_unique<query::TriggerStore>(config.durability.storage_directory / "triggers");
  std::unique_ptr<storage::PlanInvalidator> invalidator = std::make_unique<PlanInvalidatorForDatabase>(plan_cache_);
  auto database_protector_factory = [this]() -> storage::DatabaseProtectorPtr {
    if (auto db_acc = gatekeeper_ref_.load(std::memory_order_acquire).access()) {
      return std::make_unique<DatabaseProtector>(*std::move(db_acc));
    }
    return nullptr;
  };

  // Bound the per-DB cap by the global --memory-limit; SetHardLimit(0) falls back to it.
  if (auto global_max = utils::total_memory_tracker.MaximumHardLimit(); global_max > 0) {
    db_total_memory_tracker_.SetMaximumHardLimit(global_max);
    db_total_memory_tracker_.SetHardLimit(0);
  }
  if (config.tenant_memory_limit && *config.tenant_memory_limit > 0) {
    db_total_memory_tracker_.SetHardLimit(*config.tenant_memory_limit);
  }

  if (config.salient.storage_mode == memgraph::storage::StorageMode::ON_DISK_TRANSACTIONAL || config.force_on_disk ||
      utils::DirExists(config.disk.main_storage_directory)) {
    config.salient.storage_mode = memgraph::storage::StorageMode::ON_DISK_TRANSACTIONAL;
    storage_ = std::make_unique<storage::DiskStorage>(std::move(config),
                                                      std::move(invalidator),
                                                      metrics_.handles(),
                                                      database_protector_factory,
                                                      db_arena_.get(),
                                                      &db_embedding_memory_tracker_);
  } else {
    storage_ = dbms::CreateInMemoryStorage(std::move(config),
                                           std::move(invalidator),
                                           metrics_.handles(),
                                           database_protector_factory,
                                           db_arena_.get(),
                                           &db_embedding_memory_tracker_);
  }

  // Recovery adopts the uuid of the snapshot or WAL it recovered from, which need not be the one the
  // registration was made under.
  metrics_.Rebind(storage_->uuid());
  streams_->BindStorageUuid(storage_->uuid());
}

DatabaseInfo Database::GetInfo() const {
  DatabaseInfo info;
  info.storage_info = storage_->GetInfo();
  info.triggers = trigger_store_->GetTriggerInfo().size();
  info.streams = streams_->GetStreamInfo().size();
  info.db_memory_tracked = DbMemoryUsage();
  info.db_peak_memory_tracked = DbPeakMemoryUsage();
  info.db_storage_memory_tracked = DbStorageMemoryUsage();
  info.db_embedding_memory_tracked = DbEmbeddingMemoryUsage();
  info.db_query_memory_tracked = DbQueryMemoryUsage();
  return info;
}

void Database::AddTask(utils::ThreadPool::TaskSignature new_task) {
  if (!after_commit_trigger_pool_.AddTask(std::move(new_task))) {
    spdlog::warn(
        "Database '{}': could not schedule an after commit trigger because the database's background tasks are "
        "already shut down (database is being dropped or the process is shutting down); the trigger will not run.",
        name());
  }
}

void Database::StopAllBackgroundTasks() {
  streams()->Shutdown();
  // Signal any in-flight after-commit trigger to abort, so the pool join below cannot block on a
  // long-running trigger that holds an accessor pinning this database.
  after_commit_trigger_status_.store(query::TransactionStatus::TERMINATED, std::memory_order_release);
  auto const discarded = thread_pool()->ShutDown();
  if (discarded != 0) {
    spdlog::warn(
        "Database '{}': shutting down background tasks discarded {} queued after commit trigger execution(s); "
        "these triggers will not run.",
        name(),
        discarded);
  }
  storage_->StopAllBackgroundTasks();
}

void Database::SwitchToOnDisk() {
  std::vector<std::string> label_index_names;
  std::vector<std::pair<std::string, std::string>> label_prop_index_names;
  std::vector<std::pair<std::string, std::string>> existence_constraint_names;
  std::vector<std::pair<std::string, std::vector<std::string>>> unique_constraint_names;
  std::vector<std::string> unsupported_items;

  {
    auto old_acc = storage_->ReadOnlyAccess();
    const auto indices = old_acc->ListAllIndices();
    const auto constraints = old_acc->ListAllConstraints();

    for (const auto &lbl : indices.label) {
      label_index_names.push_back(storage_->LabelToName(lbl));
    }
    for (const auto &entry : indices.label_properties) {
      const auto &lbl_name = storage_->LabelToName(entry.label);
      if (entry.order != storage::IndexOrder::ASC || entry.properties.size() != 1 || entry.properties[0].size() != 1) {
        unsupported_items.push_back(fmt::format("label-property index on :{}", lbl_name));
      } else {
        label_prop_index_names.emplace_back(lbl_name, storage_->PropertyToName(entry.properties[0][0]));
      }
    }
    for (const auto &et : indices.edge_type) {
      unsupported_items.push_back(fmt::format("edge-type index on :{}", storage_->EdgeTypeToName(et)));
    }
    for (const auto &[et, prop] : indices.edge_type_property) {
      unsupported_items.push_back(
          fmt::format("edge-type-property index on :{}({})", storage_->EdgeTypeToName(et), storage_->PropertyToName(prop)));
    }
    for (const auto &prop : indices.edge_property) {
      unsupported_items.push_back(fmt::format("global edge-property index on ({})", storage_->PropertyToName(prop)));
    }
    for (const auto &prop : indices.vertex_property) {
      unsupported_items.push_back(fmt::format("global vertex-property index on ({})", storage_->PropertyToName(prop)));
    }
    for (const auto &spec : indices.text_indices) {
      unsupported_items.push_back(fmt::format("text index '{}'", spec.index_name));
    }
    for (const auto &spec : indices.text_edge_indices) {
      unsupported_items.push_back(fmt::format("edge text index '{}'", spec.index_name));
    }
    for (const auto &[lbl, prop] : indices.point_label_property) {
      unsupported_items.push_back(
          fmt::format("point index on :{}({})", storage_->LabelToName(lbl), storage_->PropertyToName(prop)));
    }
    for (const auto &spec : indices.vector_indices_spec) {
      unsupported_items.push_back(fmt::format("vector index '{}'", spec.index_name));
    }
    for (const auto &spec : indices.vector_edge_indices_spec) {
      unsupported_items.push_back(fmt::format("edge vector index '{}'", spec.index_name));
    }

    for (const auto &[lbl, prop] : constraints.existence) {
      existence_constraint_names.emplace_back(storage_->LabelToName(lbl), storage_->PropertyToName(prop));
    }
    for (const auto &[lbl, props] : constraints.unique) {
      std::vector<std::string> prop_names;
      prop_names.reserve(props.size());
      for (const auto &prop : props) {
        prop_names.push_back(storage_->PropertyToName(prop));
      }
      unique_constraint_names.emplace_back(storage_->LabelToName(lbl), std::move(prop_names));
    }
    for (const auto &[lbl, prop, kind] : constraints.type) {
      (void)kind;
      unsupported_items.push_back(
          fmt::format("property-type constraint on :{}({})", storage_->LabelToName(lbl), storage_->PropertyToName(prop)));
    }
  }

  if (!unsupported_items.empty()) {
    std::string joined;
    for (size_t i = 0; i < unsupported_items.size(); ++i) {
      if (i > 0) joined += ", ";
      joined += unsupported_items[i];
    }
    throw utils::BasicException(
        "Cannot switch to on-disk storage mode because the following schema definitions are not supported on disk and "
        "would be lost: {}. Drop them first and retry.",
        joined);
  }

  // Clear cached plans tied to the previous storage's label/property IDs and bind the replacement backend to plan_cache_.
  plan_cache_.WithLock([](query::PlanCache_t &cache) { cache.reset(); });

  // Preserve the database protector factory from the previous storage
  // This ensures consistent behavior for async operations (indexer, TTL) across storage transitions
  auto preserved_factory = storage_->get_database_protector_factory();
  const memory::DbArenaScope db_arena_scope{this};
  auto next_config = std::move(storage_->config_);
  next_config.salient.storage_mode = storage::StorageMode::ON_DISK_TRANSACTIONAL;
  storage_ = std::make_unique<memgraph::storage::DiskStorage>(std::move(next_config),
                                                              std::make_unique<PlanInvalidatorForDatabase>(plan_cache_),
                                                              metrics_.handles(),
                                                              preserved_factory,
                                                              db_arena_.get(),
                                                              &db_embedding_memory_tracker_);
  streams_->BindStorageUuid(storage_->uuid());

  if (!label_index_names.empty() || !label_prop_index_names.empty() || !existence_constraint_names.empty() ||
      !unique_constraint_names.empty()) {
    auto disk_acc = storage_->UniqueAccess();
    for (const auto &lbl_name : label_index_names) {
      if (!disk_acc->CreateIndex(storage_->NameToLabel(lbl_name)).has_value()) {
        throw utils::BasicException("Failed to preserve label index on :{} while switching to on-disk storage mode.",
                                    lbl_name);
      }
    }
    for (const auto &[lbl_name, prop_name] : label_prop_index_names) {
      storage::PropertiesPaths paths{storage::PropertyPath{storage_->NameToProperty(prop_name)}};
      if (!disk_acc->CreateIndex(storage_->NameToLabel(lbl_name), std::move(paths), storage::IndexOrder::ASC).has_value()) {
        throw utils::BasicException(
            "Failed to preserve label-property index on :{}({}) while switching to on-disk storage mode.",
            lbl_name,
            prop_name);
      }
    }
    for (const auto &[lbl_name, prop_name] : existence_constraint_names) {
      if (!disk_acc->CreateExistenceConstraint(storage_->NameToLabel(lbl_name), storage_->NameToProperty(prop_name))
               .has_value()) {
        throw utils::BasicException(
            "Failed to preserve existence constraint on :{}({}) while switching to on-disk storage mode.",
            lbl_name,
            prop_name);
      }
    }
    for (const auto &[lbl_name, prop_names] : unique_constraint_names) {
      std::set<storage::PropertyId> prop_ids;
      for (const auto &prop_name : prop_names) {
        prop_ids.insert(storage_->NameToProperty(prop_name));
      }
      if (!disk_acc->CreateUniqueConstraint(storage_->NameToLabel(lbl_name), prop_ids).has_value()) {
        throw utils::BasicException(
            "Failed to preserve uniqueness constraint on :{} while switching to on-disk storage mode.", lbl_name);
      }
    }
    if (!disk_acc->PrepareForCommitPhase(storage::CommitArgs::make_main(nullptr)).has_value()) {
      throw utils::BasicException("Failed to persist schema metadata while switching to on-disk storage mode.");
    }
    disk_acc->FinalizeTransaction();
  }
}

}  // namespace memgraph::dbms

// DbArenaScope constructor implementation (Database* variant) - defined here
// to avoid circular include between db_arena.cpp and database.hpp
namespace memgraph::memory {
DbArenaScope::DbArenaScope(const memgraph::dbms::Database *db) : DbArenaScope(db != nullptr ? &db->Arena() : nullptr) {}
}  // namespace memgraph::memory
