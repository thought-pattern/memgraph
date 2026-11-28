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

#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#ifdef MG_ENTERPRISE
#include "kvstore/kvstore.hpp"
#endif

namespace memgraph::dbms {

class TenantProfiles {
 public:
  struct Profile {
    std::string name;
    int64_t memory_limit{0};  // bytes, 0 = unlimited
    std::unordered_set<std::string> databases;
    friend bool operator==(const Profile &, const Profile &) = default;
  };

#ifdef MG_ENTERPRISE
  static constexpr std::string_view kPrefix = "tenant_profile:";
  static constexpr std::string_view kDbMappingPrefix = "db_tenant_profile:";
  static constexpr std::string_view kVersionKey = "tenant_profiles_version";
  static constexpr std::string_view kVersion = "V1";

  explicit TenantProfiles(kvstore::KVStore &durability);

  enum class CreateError : uint8_t { ALREADY_EXISTS, DURABILITY_ERROR };
  enum class AlterError : uint8_t { NOT_FOUND, DURABILITY_ERROR };
  enum class DropError : uint8_t { NOT_FOUND, HAS_ATTACHED_DATABASES, DURABILITY_ERROR };
  enum class AttachError : uint8_t { PROFILE_NOT_FOUND, DURABILITY_ERROR };
  enum class DetachError : uint8_t { NOT_ATTACHED, DURABILITY_ERROR };
  enum class RenameError : uint8_t { NOT_ATTACHED, DURABILITY_ERROR };
  enum class LimitLookupError : uint8_t { DURABILITY_ERROR };

  std::expected<void, CreateError> Create(std::string_view name, int64_t memory_limit);
  std::expected<std::unordered_set<std::string>, AlterError> Alter(std::string_view name, int64_t memory_limit);
  std::expected<void, DropError> Drop(std::string_view name);

  std::optional<Profile> Get(std::string_view name) const;
  std::vector<Profile> GetAll() const;
  std::vector<Profile> GetAllStrict() const;
  [[nodiscard]] bool ReconcileForRecovery(const std::vector<Profile> &incoming);

  /// Resolves the configured memory limit for `db_name`, distinguishing "no profile attached" (`std::nullopt`)
  /// from an unreadable mapping key or unreadable/corrupt profile record (`LimitLookupError::DURABILITY_ERROR`).
  std::expected<std::optional<int64_t>, LimitLookupError> ResolveDatabaseLimit(std::string_view db_name) const;

  std::expected<int64_t, AttachError> AttachToDatabase(std::string_view profile_name, std::string_view db_name);

  /// Detaches `db_name` from whichever profile it is attached to. `extra_keys_to_delete` are folded into
  /// the SAME atomic kvstore batch as the detach itself and are deleted if and only if the function
  /// returns success; on ANY error -- including NOT_ATTACHED -- none of them are deleted and the caller
  /// remains responsible for them.
  std::expected<void, DetachError> DetachFromDatabase(std::string_view db_name,
                                                      std::vector<std::string> extra_keys_to_delete = {});
  std::expected<void, RenameError> RenameDatabase(std::string_view old_name, std::string_view new_name,
                                                  std::map<std::string, std::string> extra_puts = {},
                                                  std::vector<std::string> extra_deletes = {});

  /// Startup reconciliation. A DROP that retires the tenant's durability key without its detach -- the
  /// detach refuses on a corrupt profile record and the caller retires the key anyway, and older
  /// releases wrote the two non-atomically -- leaves a permanently stale kDbMappingPrefix entry (and a
  /// dangling name inside the pointed-to Profile's `databases`) that nothing else ever reconciles. This
  /// removes every persisted database->profile attachment whose database name is absent from
  /// `live_db_names`: the mapping key is deleted and the name is dropped from its profile's `databases`,
  /// atomically. Returns the number pruned; 0 is ambiguous between nothing-to-prune and a failed
  /// durability batch (logged here at error level), so a caller must not read 0 as success.
  std::size_t PruneDatabases(const std::set<std::string> &live_db_names);

  std::optional<std::string> GetProfileForDatabase(std::string_view db_name) const;

 private:
  static nlohmann::json ProfileToJson(const Profile &profile);

  mutable std::shared_mutex mutex_;
  kvstore::KVStore *durability_;
#endif
};

}  // namespace memgraph::dbms
