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

#include "metrics.hpp"

#include <nlohmann/json.hpp>

namespace {
constexpr auto kName = "name";
constexpr auto kSupportedBoltVersions = "supported_bolt_versions";
constexpr auto kBoltVersion = "bolt_version";
constexpr auto kConnectionTypes = "connection_types";
constexpr auto kSessions = "sessions";
constexpr auto kQueries = "queries";
}  // namespace

namespace memgraph::communication {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
BoltMetrics bolt_metrics;

nlohmann::json BoltMetrics::Info::ToJson() const {
  nlohmann::json res;

  res[kName] = name;
  res[kSupportedBoltVersions] = nlohmann::json::array();
  {
    std::lock_guard lock{version_mtx_};
    for (const auto &sbv : supported_bolt_v_) {
      res[kSupportedBoltVersions].push_back(sbv);
    }
    res[kBoltVersion] = bolt_v_;
  }
  res[kConnectionTypes] = {{ConnectionTypeStr((ConnectionType)0), connection_types[0].load()},
                           {ConnectionTypeStr((ConnectionType)1), connection_types[1].load()}};
  res[kSessions] = sessions.load();
  res[kQueries] = queries.load();
  return res;
}

nlohmann::json BoltMetrics::ToJson() {
  auto l = std::unique_lock{mtx};
  auto res = nlohmann::json::array();
  for (const auto &[_, client_info] : info) {
    res.push_back(client_info.ToJson());
  }
  if (aggregated_other_.sessions.load() > 0 || aggregated_other_.queries.load() > 0 ||
      aggregated_other_.ActiveSessions() > 0) {
    res.push_back(aggregated_other_.ToJson());
  }
  return res;
}
}  // namespace memgraph::communication
