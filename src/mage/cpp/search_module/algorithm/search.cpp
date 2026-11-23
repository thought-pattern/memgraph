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

#include "algorithm/search.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Search {
namespace {

// One label and the properties searched for it. A property list is a disjunction (matches are unioned).
using LabelProperties = std::vector<std::pair<std::string, std::vector<std::string>>>;

constexpr std::string_view kWhitespace = " \t\n\r\f\v";

// Canonical Cypher operators emitted into the sub-query. STARTS WITH / ENDS WITH / CONTAINS are string-only.
namespace op {
constexpr std::string_view kEqual = "=";
constexpr std::string_view kNotEqual = "<>";
constexpr std::string_view kLess = "<";
constexpr std::string_view kLessEqual = "<=";
constexpr std::string_view kGreater = ">";
constexpr std::string_view kGreaterEqual = ">=";
constexpr std::string_view kStartsWith = "STARTS WITH";
constexpr std::string_view kEndsWith = "ENDS WITH";
constexpr std::string_view kContains = "CONTAINS";
constexpr std::string_view kRegex = "=~";
}  // namespace op

std::string NormalizeOperator(std::string_view raw) {
  const auto begin = raw.find_first_not_of(kWhitespace);
  if (begin == std::string_view::npos) return "";
  const auto end = raw.find_last_not_of(kWhitespace);
  std::string normalized{raw.substr(begin, end - begin + 1)};
  std::ranges::transform(
      normalized, normalized.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return normalized;
}

std::string_view ComparisonOperator(std::string_view raw) {
  const std::string normalized = NormalizeOperator(raw);
  if (normalized == "=" || normalized == "exact") return op::kEqual;
  if (normalized == "<>") return op::kNotEqual;
  if (normalized == "<") return op::kLess;
  if (normalized == "<=") return op::kLessEqual;
  if (normalized == ">") return op::kGreater;
  if (normalized == ">=") return op::kGreaterEqual;
  if (normalized == "starts with") return op::kStartsWith;
  if (normalized == "ends with") return op::kEndsWith;
  if (normalized == "contains") return op::kContains;
  if (normalized == "=~") return op::kRegex;
  throw mgp::ValueException(
      "operator `" + normalized +
      "` invalid, it must be one of (case insensitive): [<=, =~, contains, <>, ends with, starts with, exact, <, =, "
      ">, >=].");
}

bool PropertyMatches(const mgp::Value &prop_value, std::string_view comparison, std::string_view target,
                     const std::optional<std::regex> &compiled_regex) {
  if (prop_value.IsNull()) {
    return false;
  }
  if (!prop_value.IsString()) {
    return comparison == op::kNotEqual;
  }

  const std::string_view candidate = prop_value.ValueString();
  if (comparison == op::kEqual) return candidate == target;
  if (comparison == op::kNotEqual) return candidate != target;
  if (comparison == op::kLess) return candidate < target;
  if (comparison == op::kLessEqual) return candidate <= target;
  if (comparison == op::kGreater) return candidate > target;
  if (comparison == op::kGreaterEqual) return candidate >= target;
  if (comparison == op::kStartsWith) return candidate.starts_with(target);
  if (comparison == op::kEndsWith) return candidate.ends_with(target);
  if (comparison == op::kContains) return candidate.find(target) != std::string_view::npos;
  if (comparison == op::kRegex) {
    return compiled_regex.has_value() && std::regex_match(candidate.begin(), candidate.end(), *compiled_regex);
  }
  return false;
}

LabelProperties ParseJsonLabelPropertyMap(std::string_view text) {
  nlohmann::json json;
  try {
    json = nlohmann::json::parse(text);
  } catch (const nlohmann::json::parse_error &e) {
    throw mgp::ValueException(std::string{"label_property_map: malformed JSON: "} + e.what());
  }
  if (!json.is_object()) throw mgp::ValueException("label_property_map JSON must be an object");
  LabelProperties result;
  for (const auto &[label, value] : json.items()) {
    std::vector<std::string> properties;
    if (value.is_string()) {
      properties.emplace_back(value.get<std::string>());
    } else if (value.is_array()) {
      for (const auto &element : value) {
        if (!element.is_string()) throw mgp::ValueException("label_property_map values must be strings");
        properties.emplace_back(element.get<std::string>());
      }
    } else {
      throw mgp::ValueException("label_property_map values must be a string or a list of strings");
    }
    result.emplace_back(label, std::move(properties));
  }
  return result;
}

LabelProperties ParseLabelPropertyMap(const mgp::Value &argument) {
  if (argument.IsNull()) {
    throw mgp::ValueException(R"(label_property_map cannot be null. Example: {Person: ["name"], Company: "name"})");
  }
  if (argument.IsString()) return ParseJsonLabelPropertyMap(argument.ValueString());
  if (!argument.IsMap()) {
    throw mgp::ValueException("label_property_map must be a map or a JSON string");
  }
  LabelProperties result;
  for (const auto item : argument.ValueMap()) {
    std::vector<std::string> properties;
    if (item.value.IsString()) {
      properties.emplace_back(item.value.ValueString());
    } else if (item.value.IsList()) {
      for (const auto property : item.value.ValueList()) {
        if (!property.IsString()) throw mgp::ValueException("label_property_map values must be strings");
        properties.emplace_back(property.ValueString());
      }
    } else {
      throw mgp::ValueException("label_property_map values must be a string or a list of strings");
    }
    result.emplace_back(std::string{item.key}, std::move(properties));
  }
  return result;
}

// Shared driver. `deduplicate` distinguishes `node` (dedup by id) from `node_all`; evaluated directly on the
// caller's graph snapshot so uncommitted caller edits are visible and concurrent commits cannot leak across snapshots.
void Run(mgp_list *args, mgp_graph *memgraph_graph, mgp_result *result, mgp_memory *memory, bool deduplicate) {
  const mgp::MemoryDispatcherGuard guard{memory};
  const auto arguments = mgp::List(args);
  const auto record_factory = mgp::RecordFactory(result);
  try {
    const auto label_properties = ParseLabelPropertyMap(arguments[0]);
    const std::string_view comparison = ComparisonOperator(arguments[1].ValueString());
    if (arguments[2].IsNull()) return;  // a null value matches nothing (checked after operator validation)
    const std::string_view value = arguments[2].ValueString();

    std::optional<std::regex> compiled_regex;
    if (comparison == op::kRegex) {
      try {
        compiled_regex.emplace(std::string(value));
      } catch (const std::regex_error &e) {
        throw mgp::ValueException(std::string{"Regex error in '"} + std::string(value) + "': " + e.what());
      }
    }

    std::unordered_set<int64_t> seen;
    const mgp::Graph graph{memgraph_graph};
    for (const auto &[label, properties] : label_properties) {
      for (const auto &property : properties) {
        for (const auto &node : graph.Nodes()) {
          if (mgp::must_abort(memgraph_graph)) [[unlikely]] {
            throw mgp::TerminatedMustAbortException();
          }
          if (!node.HasLabel(label)) {
            continue;
          }
          if (!PropertyMatches(node.GetProperty(property), comparison, value, compiled_regex)) {
            continue;
          }
          if (deduplicate && !seen.insert(node.Id().AsInt()).second) {
            continue;
          }
          auto record = record_factory.NewRecord();
          record.Insert(kResultNode, node);
        }
      }
    }
  } catch (const std::exception &e) {
    record_factory.SetErrorMessage(e.what());
    return;
  }
}

}  // namespace

void Node(mgp_list *args, mgp_graph *memgraph_graph, mgp_result *result, mgp_memory *memory) {
  Run(args, memgraph_graph, result, memory, /*deduplicate=*/true);
}

void NodeAll(mgp_list *args, mgp_graph *memgraph_graph, mgp_result *result, mgp_memory *memory) {
  Run(args, memgraph_graph, result, memory, /*deduplicate=*/false);
}

}  // namespace Search
