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

#include "query/parse_config_map.hpp"

#include <algorithm>
#include <utility>

#include <range/v3/range/conversion.hpp>
#include <range/v3/view/all.hpp>
#include <range/v3/view/transform.hpp>
#include "spdlog/spdlog.h"

#include "query/frontend/ast/ast.hpp"
#include "query/typed_value.hpp"

namespace rv = ranges::views;

namespace memgraph::query {

auto ParseConfigMap(std::vector<std::pair<Expression *, Expression *>> const &config_map,
                    ExpressionVisitor<TypedValue> &evaluator)
    -> std::optional<std::map<std::string, std::string, std::less<>>> {
  std::map<std::string, std::string, std::less<>> parsed;
  for (const auto &[key_node, value_node] : config_map) {
    auto key_expr = key_node->Accept(evaluator);
    auto value_expr = value_node->Accept(evaluator);
    if (!key_expr.IsString() || !value_expr.IsString()) {
      spdlog::error("Config map must contain only string keys and values!");
      return std::nullopt;
    }
    std::string key_str{key_expr.ValueString()};
    if (!parsed.emplace(key_str, std::string{value_expr.ValueString()}).second) {
      spdlog::error("Duplicate key '{}' in config map!", key_str);
      return std::nullopt;
    }
  }
  return parsed;
}

}  // namespace memgraph::query
