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

#include "dbms/tenant_profiles.hpp"

#ifdef MG_ENTERPRISE

#include <iterator>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "spdlog/spdlog.h"
#include "utils/exceptions.hpp"

namespace memgraph::dbms {

namespace {

auto ProfileKey(std::string_view name) -> std::string {
  return std::string{TenantProfiles::kPrefix} + std::string{name};
}

auto DbMappingKey(std::string_view db) -> std::string {
  return std::string{TenantProfiles::kDbMappingPrefix} + std::string{db};
}

auto FromJson(const nlohmann::json &json, std::string_view name) -> TenantProfiles::Profile {
  if (!json.is_object()) {
    (void)json.get<std::map<std::string, nlohmann::json>>();
  }
  TenantProfiles::Profile profile;
  profile.name = name;
  profile.memory_limit = json.at("memory_limit").get<int64_t>();
  if (profile.memory_limit < 0) {
    throw utils::BasicException("Tenant profile '{}' has negative memory_limit {}.", name, profile.memory_limit);
  }
  profile.databases = json.at("databases").get<std::unordered_set<std::string>>();
  return profile;
}

}  // namespace

nlohmann::json TenantProfiles::ProfileToJson(const Profile &profile) {
  nlohmann::json json;
  json["memory_limit"] = profile.memory_limit;
  json["databases"] = profile.databases;
  return json;
}

TenantProfiles::TenantProfiles(kvstore::KVStore &durability) : durability_{&durability} {
  auto existing_version = durability_->GetStrict(kVersionKey);
  if (existing_version.has_value() && *existing_version == kVersion) {
    return;
  }
  if (!existing_version.has_value() && existing_version.error() == kvstore::KVStore::GetError::kReadError) {
    spdlog::warn("Failed to read tenant profiles version key during startup.");
    return;
  }
  durability_->Put(kVersionKey, kVersion);
}

std::expected<void, TenantProfiles::CreateError> TenantProfiles::Create(std::string_view name, int64_t memory_limit) {
  const std::unique_lock lock{mutex_};
  auto existing = durability_->GetStrict(ProfileKey(name));
  if (existing.has_value()) {
    return std::unexpected{CreateError::ALREADY_EXISTS};
  }
  if (existing.error() != kvstore::KVStore::GetError::kNotFound) {
    return std::unexpected{CreateError::DURABILITY_ERROR};
  }

  const Profile profile{.name = std::string{name}, .memory_limit = memory_limit};
  if (!durability_->Put(ProfileKey(profile.name), ProfileToJson(profile).dump())) {
    return std::unexpected{CreateError::DURABILITY_ERROR};
  }
  return {};
}

std::expected<std::unordered_set<std::string>, TenantProfiles::AlterError> TenantProfiles::Alter(std::string_view name,
                                                                                                 int64_t memory_limit) {
  const std::unique_lock lock{mutex_};
  auto stored = durability_->GetStrict(ProfileKey(name));
  if (!stored.has_value()) {
    return std::unexpected{stored.error() == kvstore::KVStore::GetError::kNotFound ? AlterError::NOT_FOUND
                                                                                   : AlterError::DURABILITY_ERROR};
  }

  Profile profile;
  try {
    profile = FromJson(nlohmann::json::parse(*stored), name);
  } catch (const nlohmann::json::exception &e) {
    spdlog::warn("Failed to parse tenant profile '{}' during alter: {}", name, e.what());
    return std::unexpected{AlterError::DURABILITY_ERROR};
  } catch (const utils::BasicException &e) {
    spdlog::warn("Invalid tenant profile '{}' during alter: {}", name, e.what());
    return std::unexpected{AlterError::DURABILITY_ERROR};
  }
  profile.memory_limit = memory_limit;
  if (!durability_->Put(ProfileKey(profile.name), ProfileToJson(profile).dump())) {
    return std::unexpected{AlterError::DURABILITY_ERROR};
  }
  return profile.databases;
}

std::expected<void, TenantProfiles::DropError> TenantProfiles::Drop(std::string_view name) {
  const std::unique_lock lock{mutex_};
  auto stored = durability_->GetStrict(ProfileKey(name));
  if (!stored.has_value()) {
    return std::unexpected{stored.error() == kvstore::KVStore::GetError::kNotFound ? DropError::NOT_FOUND
                                                                                   : DropError::DURABILITY_ERROR};
  }

  Profile profile;
  try {
    profile = FromJson(nlohmann::json::parse(*stored), name);
  } catch (const nlohmann::json::exception &e) {
    spdlog::warn("Failed to parse tenant profile '{}' during drop: {}", name, e.what());
    return std::unexpected{DropError::DURABILITY_ERROR};
  } catch (const utils::BasicException &e) {
    spdlog::warn("Invalid tenant profile '{}' during drop: {}", name, e.what());
    return std::unexpected{DropError::DURABILITY_ERROR};
  }
  if (!profile.databases.empty()) return std::unexpected{DropError::HAS_ATTACHED_DATABASES};

  if (!durability_->Delete(ProfileKey(name))) return std::unexpected{DropError::DURABILITY_ERROR};
  return {};
}

std::optional<TenantProfiles::Profile> TenantProfiles::Get(std::string_view name) const {
  const std::shared_lock lock{mutex_};
  auto stored = durability_->GetStrict(ProfileKey(name));
  if (!stored.has_value()) return std::nullopt;
  try {
    return FromJson(nlohmann::json::parse(*stored), name);
  } catch (const nlohmann::json::exception &e) {
    spdlog::warn("Failed to parse tenant profile '{}': {}", name, e.what());
    return std::nullopt;
  } catch (const utils::BasicException &e) {
    spdlog::warn("Invalid tenant profile '{}': {}", name, e.what());
    return std::nullopt;
  }
}

std::vector<TenantProfiles::Profile> TenantProfiles::GetAll() const {
  const std::shared_lock lock{mutex_};
  std::vector<Profile> result;
  for (auto it = durability_->begin(std::string{kPrefix}); it != durability_->end(std::string{kPrefix}); ++it) {
    const auto &[key, value] = *it;
    auto name = key.substr(kPrefix.size());
    try {
      result.push_back(FromJson(nlohmann::json::parse(value), name));
    } catch (const nlohmann::json::exception &e) {
      spdlog::warn("Failed to parse tenant profile '{}': {}", name, e.what());
    } catch (const utils::BasicException &e) {
      spdlog::warn("Invalid tenant profile '{}': {}", name, e.what());
    }
  }
  return result;
}

std::vector<TenantProfiles::Profile> TenantProfiles::GetAllStrict() const {
  const std::shared_lock lock{mutex_};
  std::vector<Profile> result;
  auto it = durability_->begin(std::string{kPrefix});
  const auto end_it = durability_->end(std::string{kPrefix});
  for (; it != end_it; ++it) {
    const auto &[key, value] = *it;
    const auto name = key.substr(kPrefix.size());
    try {
      result.push_back(FromJson(nlohmann::json::parse(value), name));
    } catch (const nlohmann::json::exception &e) {
      throw utils::BasicException("Failed to decode tenant profile '{}' for recovery snapshot: {}", name, e.what());
    }
  }
  if (it.HasError()) {
    throw utils::BasicException("Failed to scan tenant profiles for recovery snapshot: {}", it.ErrorMessage());
  }
  return result;
}

bool TenantProfiles::ReconcileForRecovery(const std::vector<Profile> &incoming) {
  const std::unique_lock lock{mutex_};
  std::map<std::string, std::string> desired_puts;
  for (const auto &profile : incoming) {
    if (profile.memory_limit < 0) {
      spdlog::error("Rejecting tenant profile '{}' with negative memory_limit {} during recovery.",
                    profile.name,
                    profile.memory_limit);
      return false;
    }
    desired_puts.insert_or_assign(ProfileKey(profile.name), ProfileToJson(profile).dump());
    for (const auto &db_name : profile.databases) {
      desired_puts.insert_or_assign(DbMappingKey(db_name), profile.name);
    }
  }

  std::vector<std::string> to_delete;
  auto prof_it = durability_->begin(std::string{kPrefix});
  const auto prof_end = durability_->end(std::string{kPrefix});
  for (; prof_it != prof_end; ++prof_it) {
    if (!desired_puts.contains(prof_it->first)) {
      to_delete.push_back(prof_it->first);
    }
  }
  if (prof_it.HasError()) {
    spdlog::error("Failed to scan existing tenant profiles during replica recovery: {}", prof_it.ErrorMessage());
    return false;
  }

  auto map_it = durability_->begin(std::string{kDbMappingPrefix});
  const auto map_end = durability_->end(std::string{kDbMappingPrefix});
  for (; map_it != map_end; ++map_it) {
    if (!desired_puts.contains(map_it->first)) {
      to_delete.push_back(map_it->first);
    }
  }
  if (map_it.HasError()) {
    spdlog::error("Failed to scan existing database tenant profile mappings during replica recovery: {}",
                  map_it.ErrorMessage());
    return false;
  }

  if (desired_puts.empty() && to_delete.empty()) {
    return true;
  }
  if (!durability_->PutAndDeleteMultiple(desired_puts, to_delete)) {
    spdlog::error("Failed to commit tenant profile reconciliation batch during replica recovery.");
    return false;
  }
  return true;
}

std::expected<std::optional<int64_t>, TenantProfiles::LimitLookupError> TenantProfiles::ResolveDatabaseLimit(
    std::string_view db_name) const {
  const std::shared_lock lock{mutex_};
  auto mapped = durability_->GetStrict(DbMappingKey(db_name));
  if (mapped.has_value()) {
    auto stored = durability_->GetStrict(ProfileKey(*mapped));
    if (!stored.has_value()) {
      spdlog::error("Tenant profile '{}' assigned to database '{}' could not be loaded.", *mapped, db_name);
      return std::unexpected{LimitLookupError::DURABILITY_ERROR};
    }
    try {
      const Profile profile = FromJson(nlohmann::json::parse(*stored), *mapped);
      return std::optional<int64_t>{profile.memory_limit};
    } catch (const nlohmann::json::exception &e) {
      spdlog::error("Tenant profile '{}' assigned to database '{}' is corrupt ({}).", *mapped, db_name, e.what());
      return std::unexpected{LimitLookupError::DURABILITY_ERROR};
    } catch (const utils::BasicException &e) {
      spdlog::error("Tenant profile '{}' assigned to database '{}' is invalid ({}).", *mapped, db_name, e.what());
      return std::unexpected{LimitLookupError::DURABILITY_ERROR};
    }
  }
  if (mapped.error() != kvstore::KVStore::GetError::kNotFound) {
    spdlog::error("Failed to read tenant profile assignment for database '{}'.", db_name);
    return std::unexpected{LimitLookupError::DURABILITY_ERROR};
  }

  // Fallback check for member-list-only attachment when db_tenant_profile:<db> is absent.
  auto it = durability_->begin(std::string{kPrefix});
  const auto end_it = durability_->end(std::string{kPrefix});
  const std::string target{db_name};
  for (; it != end_it; ++it) {
    const auto &[key, value] = *it;
    const auto name = key.substr(kPrefix.size());
    bool mentions_target = false;
    try {
      const auto parsed = nlohmann::json::parse(value);
      if (parsed.is_object() && parsed.contains("databases") && parsed["databases"].is_array()) {
        for (const auto &entry : parsed["databases"]) {
          if (entry.is_string() && entry.get<std::string>() == target) {
            mentions_target = true;
            break;
          }
        }
      }
      const Profile profile = FromJson(parsed, name);
      if (profile.databases.contains(target)) {
        return std::optional<int64_t>{profile.memory_limit};
      }
    } catch (const nlohmann::json::exception &e) {
      if (mentions_target) {
        spdlog::error("Tenant profile '{}' listing database '{}' is corrupt ({}).", name, db_name, e.what());
        return std::unexpected{LimitLookupError::DURABILITY_ERROR};
      }
    } catch (const utils::BasicException &e) {
      if (mentions_target) {
        spdlog::error("Tenant profile '{}' listing database '{}' is invalid ({}).", name, db_name, e.what());
        return std::unexpected{LimitLookupError::DURABILITY_ERROR};
      }
    }
  }
  if (it.HasError()) {
    return std::unexpected{LimitLookupError::DURABILITY_ERROR};
  }
  return std::optional<int64_t>{std::nullopt};
}

std::expected<int64_t, TenantProfiles::AttachError> TenantProfiles::AttachToDatabase(std::string_view profile_name,
                                                                                     std::string_view db_name) {
  const std::unique_lock lock{mutex_};
  auto new_stored = durability_->GetStrict(ProfileKey(profile_name));
  if (!new_stored.has_value()) {
    return std::unexpected{new_stored.error() == kvstore::KVStore::GetError::kNotFound ? AttachError::PROFILE_NOT_FOUND
                                                                                       : AttachError::DURABILITY_ERROR};
  }
  Profile new_profile;
  try {
    new_profile = FromJson(nlohmann::json::parse(*new_stored), profile_name);
  } catch (const nlohmann::json::exception &e) {
    spdlog::warn("Failed to parse target tenant profile '{}' during attach: {}", profile_name, e.what());
    return std::unexpected{AttachError::DURABILITY_ERROR};
  } catch (const utils::BasicException &e) {
    spdlog::warn("Invalid target tenant profile '{}' during attach: {}", profile_name, e.what());
    return std::unexpected{AttachError::DURABILITY_ERROR};
  }

  std::map<std::string, std::string> to_put;
  auto old_profile_name = durability_->GetStrict(DbMappingKey(db_name));
  if (!old_profile_name.has_value() && old_profile_name.error() != kvstore::KVStore::GetError::kNotFound) {
    spdlog::error("Failed to read previous tenant profile assignment for database '{}'.", db_name);
    return std::unexpected{AttachError::DURABILITY_ERROR};
  }
  if (old_profile_name.has_value() && *old_profile_name != profile_name) {
    auto old_stored = durability_->GetStrict(ProfileKey(*old_profile_name));
    if (!old_stored.has_value()) {
      if (old_stored.error() != kvstore::KVStore::GetError::kNotFound) {
        spdlog::error("Failed to read previous tenant profile '{}' while moving database '{}'.",
                      *old_profile_name,
                      db_name);
        return std::unexpected{AttachError::DURABILITY_ERROR};
      }
    } else {
      try {
        Profile old_profile = FromJson(nlohmann::json::parse(*old_stored), *old_profile_name);
        old_profile.databases.erase(std::string{db_name});
        to_put.emplace(ProfileKey(old_profile.name), ProfileToJson(old_profile).dump());
      } catch (const nlohmann::json::exception &e) {
        spdlog::error("Previous tenant profile '{}' is corrupt ({}); refusing to move database '{}'.",
                      *old_profile_name,
                      e.what(),
                      db_name);
        return std::unexpected{AttachError::DURABILITY_ERROR};
      } catch (const utils::BasicException &e) {
        spdlog::error("Previous tenant profile '{}' is invalid ({}); refusing to move database '{}'.",
                      *old_profile_name,
                      e.what(),
                      db_name);
        return std::unexpected{AttachError::DURABILITY_ERROR};
      }
    }
  }
  new_profile.databases.insert(std::string{db_name});
  to_put.emplace(ProfileKey(new_profile.name), ProfileToJson(new_profile).dump());
  to_put.emplace(DbMappingKey(db_name), std::string{profile_name});

  if (!durability_->PutMultiple(to_put)) return std::unexpected{AttachError::DURABILITY_ERROR};
  return new_profile.memory_limit;
}

std::expected<void, TenantProfiles::DetachError> TenantProfiles::DetachFromDatabase(
    std::string_view db_name, std::vector<std::string> extra_keys_to_delete) {
  const std::unique_lock lock{mutex_};
  auto profile_name = durability_->GetStrict(DbMappingKey(db_name));
  if (!profile_name.has_value()) {
    return std::unexpected{profile_name.error() == kvstore::KVStore::GetError::kNotFound
                               ? DetachError::NOT_ATTACHED
                               : DetachError::DURABILITY_ERROR};
  }

  auto profile_stored = durability_->GetStrict(ProfileKey(*profile_name));
  if (!profile_stored.has_value()) return std::unexpected{DetachError::DURABILITY_ERROR};

  std::map<std::string, std::string> to_put;
  try {
    Profile profile = FromJson(nlohmann::json::parse(*profile_stored), *profile_name);
    profile.databases.erase(std::string{db_name});
    to_put.emplace(ProfileKey(profile.name), ProfileToJson(profile).dump());
  } catch (const nlohmann::json::exception &e) {
    spdlog::warn("Tenant profile '{}' durable entry is corrupt ({}); failed to detach database '{}'.",
                 *profile_name,
                 e.what(),
                 db_name);
    return std::unexpected{DetachError::DURABILITY_ERROR};
  } catch (const utils::BasicException &e) {
    spdlog::warn("Tenant profile '{}' durable entry is invalid ({}); failed to detach database '{}'.",
                 *profile_name,
                 e.what(),
                 db_name);
    return std::unexpected{DetachError::DURABILITY_ERROR};
  }

  std::vector<std::string> to_delete{DbMappingKey(db_name)};
  to_delete.insert(to_delete.end(),
                   std::make_move_iterator(extra_keys_to_delete.begin()),
                   std::make_move_iterator(extra_keys_to_delete.end()));
  if (!durability_->PutAndDeleteMultiple(to_put, to_delete)) return std::unexpected{DetachError::DURABILITY_ERROR};
  return {};
}

std::size_t TenantProfiles::PruneDatabases(const std::set<std::string> &live_db_names) {
  const std::unique_lock lock{mutex_};

  std::map<std::string, std::vector<std::pair<std::string, std::string>>> stale_by_profile;
  auto it = durability_->begin(std::string{kDbMappingPrefix});
  const auto mapping_end = durability_->end(std::string{kDbMappingPrefix});
  for (; it != mapping_end; ++it) {
    const auto &[key, profile_name] = *it;
    auto db_name = key.substr(kDbMappingPrefix.size());
    if (live_db_names.contains(db_name)) continue;
    stale_by_profile[profile_name].emplace_back(key, std::move(db_name));
  }
  if (it.HasError() || stale_by_profile.empty()) return 0;

  std::map<std::string, std::string> to_put;
  std::vector<std::string> to_delete;
  for (const auto &[profile_name, stale_entries] : stale_by_profile) {
    auto stored = durability_->GetStrict(ProfileKey(profile_name));
    if (!stored.has_value()) {
      if (stored.error() == kvstore::KVStore::GetError::kNotFound) {
        for (const auto &[mapping_key, _] : stale_entries) {
          to_delete.push_back(mapping_key);
        }
      } else {
        spdlog::warn(
            "Failed to read tenant profile '{}' while pruning stale database mapping(s); retaining mapping key(s) for "
            "retry.",
            profile_name);
      }
      continue;
    }
    try {
      Profile profile = FromJson(nlohmann::json::parse(*stored), profile_name);
      for (const auto &[mapping_key, db_name] : stale_entries) {
        profile.databases.erase(db_name);
        to_delete.push_back(mapping_key);
      }
      to_put.emplace(ProfileKey(profile.name), ProfileToJson(profile).dump());
    } catch (const nlohmann::json::exception &e) {
      spdlog::warn(
          "Tenant profile '{}' durable entry is corrupt ({}); retaining its stale database mapping(s) until the "
          "profile can be reconciled.",
          profile_name,
          e.what());
    } catch (const utils::BasicException &e) {
      spdlog::warn(
          "Tenant profile '{}' durable entry is invalid ({}); retaining its stale database mapping(s) until the "
          "profile can be reconciled.",
          profile_name,
          e.what());
    }
  }

  if (to_delete.empty()) return 0;
  if (!durability_->PutAndDeleteMultiple(to_put, to_delete)) {
    spdlog::error(
        "Failed to durably prune {} stale tenant profile database attachment(s); they survive this boot and "
        "reconciliation will be retried on the next one.",
        to_delete.size());
    return 0;
  }
  return to_delete.size();
}

std::expected<void, TenantProfiles::RenameError> TenantProfiles::RenameDatabase(
    std::string_view old_name, std::string_view new_name, std::map<std::string, std::string> extra_puts,
    std::vector<std::string> extra_deletes) {
  const std::unique_lock lock{mutex_};
  auto profile_name = durability_->GetStrict(DbMappingKey(old_name));
  if (!profile_name.has_value()) {
    if (profile_name.error() != kvstore::KVStore::GetError::kNotFound) {
      return std::unexpected{RenameError::DURABILITY_ERROR};
    }
    if (!extra_puts.empty() || !extra_deletes.empty()) {
      if (!durability_->PutAndDeleteMultiple(extra_puts, extra_deletes)) {
        return std::unexpected{RenameError::DURABILITY_ERROR};
      }
      return {};
    }
    return std::unexpected{RenameError::NOT_ATTACHED};
  }

  auto profile_stored = durability_->GetStrict(ProfileKey(*profile_name));
  if (!profile_stored.has_value()) return std::unexpected{RenameError::DURABILITY_ERROR};

  std::map<std::string, std::string> to_put = std::move(extra_puts);
  try {
    Profile profile = FromJson(nlohmann::json::parse(*profile_stored), *profile_name);
    profile.databases.erase(std::string{old_name});
    profile.databases.insert(std::string{new_name});
    to_put.insert_or_assign(ProfileKey(profile.name), ProfileToJson(profile).dump());
    to_put.insert_or_assign(DbMappingKey(new_name), *profile_name);
  } catch (const nlohmann::json::exception &e) {
    spdlog::warn("Tenant profile '{}' durable entry is corrupt ({}); failed to rename database '{}' to '{}'.",
                 *profile_name,
                 e.what(),
                 old_name,
                 new_name);
    return std::unexpected{RenameError::DURABILITY_ERROR};
  } catch (const utils::BasicException &e) {
    spdlog::warn("Tenant profile '{}' durable entry is invalid ({}); failed to rename database '{}' to '{}'.",
                 *profile_name,
                 e.what(),
                 old_name,
                 new_name);
    return std::unexpected{RenameError::DURABILITY_ERROR};
  }

  std::vector<std::string> to_delete{DbMappingKey(old_name)};
  to_delete.insert(
      to_delete.end(), std::make_move_iterator(extra_deletes.begin()), std::make_move_iterator(extra_deletes.end()));
  if (!durability_->PutAndDeleteMultiple(to_put, to_delete)) return std::unexpected{RenameError::DURABILITY_ERROR};
  return {};
}

std::optional<std::string> TenantProfiles::GetProfileForDatabase(std::string_view db_name) const {
  const std::shared_lock lock{mutex_};
  return durability_->Get(DbMappingKey(db_name));
}

}  // namespace memgraph::dbms

#endif
