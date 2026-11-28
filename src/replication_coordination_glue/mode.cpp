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
#include "replication_coordination_glue/mode.hpp"

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
constexpr std::array<std::pair<ReplicationMode, std::string_view>, 3> kModeMapping{
    {{ReplicationMode::SYNC, "sync"},
     {ReplicationMode::ASYNC, "async"},
     {ReplicationMode::STRICT_SYNC, "strict_sync"}}};
}  // namespace

void to_json(nlohmann::json &j, const ReplicationMode &mode) {
  const auto *it = std::ranges::find(kModeMapping, mode, &std::pair<ReplicationMode, std::string_view>::first);
  MG_ASSERT(it != kModeMapping.end(), "Unsupported replication mode enum value");
  j = std::string(it->second);
}

void from_json(const nlohmann::json &j, ReplicationMode &mode) {
  if (!j.is_string()) {
    throw std::invalid_argument("Replication mode JSON field must be a string");
  }
  const auto value = j.get<std::string>();
  const auto *it = std::ranges::find(kModeMapping, value, &std::pair<ReplicationMode, std::string_view>::second);
  if (it == kModeMapping.end()) {
    throw std::invalid_argument("Unrecognized replication mode: " + value);
  }
  mode = it->first;
}

}  // namespace memgraph::replication_coordination_glue
