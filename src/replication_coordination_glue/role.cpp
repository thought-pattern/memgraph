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
#include "replication_coordination_glue/role.hpp"

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "utils/logging.hpp"

namespace memgraph::replication_coordination_glue {

namespace {
constexpr std::array<std::pair<ReplicationRole, std::string_view>, 2> kRoleMapping{
    {{ReplicationRole::MAIN, "main"}, {ReplicationRole::REPLICA, "replica"}}};
}  // namespace

void to_json(nlohmann::json &j, const ReplicationRole &role) {
  const auto *it = std::ranges::find(kRoleMapping, role, &std::pair<ReplicationRole, std::string_view>::first);
  MG_ASSERT(it != kRoleMapping.end(), "Unsupported replication role enum value");
  j = std::string(it->second);
}

void from_json(const nlohmann::json &j, ReplicationRole &role) {
  if (!j.is_string()) {
    throw std::invalid_argument("Replication role JSON field must be a string");
  }
  const auto value = j.get<std::string>();
  const auto *it = std::ranges::find(kRoleMapping, value, &std::pair<ReplicationRole, std::string_view>::second);
  if (it == kRoleMapping.end()) {
    throw std::invalid_argument("Unrecognized replication role: " + value);
  }
  role = it->first;
}

}  // namespace memgraph::replication_coordination_glue
