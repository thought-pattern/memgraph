// Copyright 2024 Memgraph Ltd.
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

#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>
#include "communication/bolt/v1/value.hpp"
#include "utils/credential_redaction.hpp"

namespace memgraph::communication::bolt {

enum class MgType : uint8_t {
  Enum,
};

struct mg_type_info {
  MgType type;
  std::string_view type_str;
  std::string_view value_str;
};

constexpr std::string_view kMgTypeEnum = "mg_enum";
constexpr std::string_view kMgTypeType = "__type";
constexpr std::string_view kMgTypeValue = "__value";

auto BoltMapToMgTypeInfo(map_t const &value) -> std::optional<mg_type_info>;

inline Value RedactBoltValue(const Value &value) {
  using enum Value::Type;
  switch (value.type()) {
    case String:
      if (utils::LooksLikePasswordHash(value.ValueString())) {
        return Value{utils::kRedactedMask};
      }
      return value;
    case List: {
      std::vector<Value> redacted;
      const auto &items = value.ValueList();
      redacted.reserve(items.size());
      for (const auto &item : items) {
        redacted.push_back(RedactBoltValue(item));
      }
      return Value{std::move(redacted)};
    }
    case Map: {
      const auto &bolt_map = value.ValueMap();
      if (BoltMapToMgTypeInfo(bolt_map)) {
        return value;
      }
      map_t redacted_map;
      for (const auto &[k, v] : bolt_map) {
        if (utils::IsSensitiveParamKey(k)) {
          redacted_map.emplace(k, Value{utils::kRedactedMask});
        } else {
          redacted_map.emplace(k, RedactBoltValue(v));
        }
      }
      return Value{std::move(redacted_map)};
    }
    default:
      return value;
  }
}

inline std::pair<std::string, map_t> RedactBoltQueryAndParams(std::string_view query, const map_t &params) {
  std::unordered_set<std::string> redacted_param_names;
  std::string redacted_query = utils::RedactQueryCredentials(query, &redacted_param_names);

  map_t redacted_params;
  for (const auto &[k, v] : params) {
    if (redacted_param_names.contains(k) || utils::IsSensitiveParamKey(k)) {
      redacted_params.emplace(k, Value{utils::kRedactedMask});
    } else {
      redacted_params.emplace(k, RedactBoltValue(v));
    }
  }
  return {std::move(redacted_query), std::move(redacted_params)};
}
}  // namespace memgraph::communication::bolt
