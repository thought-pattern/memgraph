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

#include "node.hpp"

#include <algorithm>
#include <ranges>
#include <unordered_set>

#include "mgp.hpp"

namespace {

std::vector<std::string> ExtractPatternStrings(const mgp::Value &raw_patterns) {
  std::vector<std::string> out;
  if (raw_patterns.IsNull()) {
    return out;
  }
  if (raw_patterns.IsList()) {
    for (const auto &elem : raw_patterns.ValueList()) {
      if (!elem.IsString()) {
        throw mgp::ValueException("Relationship pattern list elements must be strings!");
      }
      out.emplace_back(elem.ValueString());
    }
    return out;
  }
  if (raw_patterns.IsString()) {
    const std::string_view text = raw_patterns.ValueString();
    if (text.empty()) {
      return out;
    }
    std::size_t start = 0;
    while (start <= text.size()) {
      const std::size_t bar = text.find('|', start);
      if (bar == std::string_view::npos) {
        out.emplace_back(text.substr(start));
        break;
      }
      out.emplace_back(text.substr(start, bar - start));
      start = bar + 1;
    }
    return out;
  }
  throw mgp::ValueException("Relationship pattern must be a string or list of strings!");
}

bool EvaluateNodeRelationshipExists(const mgp::Node &node, std::vector<std::string> patterns) {
  if (patterns.empty()) {
    patterns.emplace_back("");
  }

  std::unordered_set<std::string> in_rels;
  std::unordered_set<std::string> out_rels;

  for (const auto &pattern : patterns) {
    if (pattern.empty()) {
      in_rels.emplace("");
      out_rels.emplace("");
      continue;
    }
    if (pattern.front() == '<' && pattern.back() == '>') {
      throw mgp::ValueException("Invalid relationship specification!");
    }
    if (pattern.front() == '<') {
      in_rels.emplace(pattern.substr(1));
      continue;
    }
    if (pattern.back() == '>') {
      out_rels.emplace(pattern.substr(0, pattern.size() - 1));
      continue;
    }
    in_rels.emplace(pattern);
    out_rels.emplace(pattern);
  }

  return Node::FindRelationship(in_rels, node.InRelationships()) ||
         Node::FindRelationship(out_rels, node.OutRelationships());
}

std::unordered_map<std::string, uint8_t> GetTypeDirection(const mgp::Value &types) {
  std::unordered_map<std::string, uint8_t> result;
  for (const auto &type_value : types.ValueList()) {
    auto type = type_value.ValueString();
    if (type.starts_with('<')) {
      if (type.ends_with('>')) {
        throw mgp::ValueException("<type> format not allowed. Use type instead.");
      }
      result[std::string(type.substr(1))] |= 1U;
    } else if (type.ends_with('>')) {
      result[std::string(type.substr(0, type.size() - 1))] |= 2U;
    } else {
      result[std::string(type)] |= 3U;
    }
  }
  return result;
}

mgp::List GetRelationshipTypes(const mgp::Value &node_value, const mgp::Value &types_value) {
  const auto type_direction = GetTypeDirection(types_value);

  std::unordered_set<std::string> types;
  const auto node = node_value.ValueNode();
  if (type_direction.empty()) {
    for (const auto relationship : node.InRelationships()) {
      types.emplace(relationship.Type());
    }
    for (const auto relationship : node.OutRelationships()) {
      types.emplace(relationship.Type());
    }
  } else {
    for (const auto relationship : node.InRelationships()) {
      const auto it = type_direction.find(std::string(relationship.Type()));
      if (it != type_direction.end() && (it->second & 1U) != 0) {
        types.emplace(relationship.Type());
      }
    }
    for (const auto relationship : node.OutRelationships()) {
      const auto it = type_direction.find(std::string(relationship.Type()));
      if (it != type_direction.end() && (it->second & 2U) != 0) {
        types.emplace(relationship.Type());
      }
    }
  }

  mgp::List result{types.size()};
  for (const auto &type : types) {
    auto value = mgp::Value(type);
    result.Append(value);
  }
  return result;
}
}  // namespace

bool Node::RelationshipExist(const mgp::Node &node, std::string &rel_type) {
  if (rel_type.empty()) {
    return node.OutRelationships().cbegin() != node.OutRelationships().cend() ||
           node.InRelationships().cbegin() != node.InRelationships().cend();
  }
  char direction{' '};
  if (rel_type.front() == '<' && rel_type.back() == '>') {
    throw mgp::ValueException("Invalid relationship specification!");
  }
  if (rel_type.back() == '>') {
    direction = rel_type.back();
    rel_type.pop_back();
  } else if (rel_type.front() == '<') {
    direction = rel_type.front();
    rel_type.erase(0, 1);
  }
  if (rel_type.empty()) {
    if (direction == '>') {
      return node.OutRelationships().cbegin() != node.OutRelationships().cend();
    }
    return node.InRelationships().cbegin() != node.InRelationships().cend();
  }
  for (auto rel : node.OutRelationships()) {
    if (rel.Type() == rel_type && direction != '<') {
      return true;
    }
  }
  return std::any_of(
      node.InRelationships().begin(), node.InRelationships().end(), [&rel_type, &direction](const auto &rel) {
        return rel.Type() == rel_type && direction != '>';
      });
}

void Node::RelationshipsExist(mgp_list *args, mgp_graph * /*memgraph_graph*/, mgp_result *result, mgp_memory *memory) {
  const mgp::MemoryDispatcherGuard guard{memory};
  const auto arguments = mgp::List(args);
  const auto record_factory = mgp::RecordFactory(result);
  try {
    const mgp::Node node = arguments[0].ValueNode();
    const mgp::List relationships = arguments[1].ValueList();
    if (relationships.Size() == 0) {
      throw mgp::ValueException("Input relationships list must not be empty!");
    }
    mgp::Map relationship_map = mgp::Map();
    for (auto rel : relationships) {
      std::string rel_type{rel.ValueString()};
      if (RelationshipExist(node, rel_type)) {
        relationship_map.Insert(rel.ValueString(), mgp::Value(true));
      } else {
        relationship_map.Insert(rel.ValueString(), mgp::Value(false));
      }
    }
    auto record = record_factory.NewRecord();
    record.Insert(std::string(kReturnRelationshipsExist).c_str(), relationship_map);

  } catch (const std::exception &e) {
    record_factory.SetErrorMessage(e.what());
    return;
  }
}

void Node::RelationshipsExistFunc(mgp_list *args, mgp_func_context * /*ctx*/, mgp_func_result *res,
                                  mgp_memory *memory) {
  const mgp::MemoryDispatcherGuard guard{memory};
  const auto arguments = mgp::List(args);
  auto result = mgp::Result(res);
  try {
    if (arguments[0].IsNull()) {
      result.SetValue(mgp::Map{});
      return;
    }
    const mgp::Node node = arguments[0].ValueNode();
    const auto patterns = ExtractPatternStrings(arguments[1]);
    if (patterns.empty()) {
      throw mgp::ValueException("Input relationships list must not be empty!");
    }
    mgp::Map relationship_map{};
    for (const auto &raw_pat : patterns) {
      std::string pat_copy = raw_pat;
      relationship_map.Update(raw_pat, mgp::Value(RelationshipExist(node, pat_copy)));
    }
    result.SetValue(std::move(relationship_map));
  } catch (const std::exception &e) {
    result.SetErrorMessage(e.what());
  }
}

bool Node::FindRelationship(const std::unordered_set<std::string> &types, mgp::Relationships relationships) {
  if (types.contains("") && relationships.cbegin() != relationships.cend()) {
    return true;
  }

  // NOLINTNEXTLINE(modernize-use-ranges, boost-use-ranges)
  return std::any_of(relationships.begin(), relationships.end(), [&types](const auto &relationship) {
    return types.contains(std::string(relationship.Type()));
  });
}

void Node::RelationshipExists(mgp_list *args, mgp_graph * /*memgraph_graph*/, mgp_result *result, mgp_memory *memory) {
  const mgp::MemoryDispatcherGuard guard{memory};
  const auto arguments = mgp::List(args);
  const auto record_factory = mgp::RecordFactory(result);
  try {
    const auto node = arguments[0].ValueNode();
    const auto patterns = ExtractPatternStrings(arguments[1]);
    auto record = record_factory.NewRecord();
    record.Insert(std::string(kResultRelationshipExists).c_str(), EvaluateNodeRelationshipExists(node, patterns));
  } catch (const std::exception &e) {
    record_factory.SetErrorMessage(e.what());
    return;
  }
}

void Node::RelationshipExistsFunc(mgp_list *args, mgp_func_context * /*ctx*/, mgp_func_result *res,
                                  mgp_memory *memory) {
  const mgp::MemoryDispatcherGuard guard{memory};
  const auto arguments = mgp::List(args);
  auto result = mgp::Result(res);
  try {
    if (arguments[0].IsNull()) {
      result.SetValue(false);
      return;
    }
    const auto node = arguments[0].ValueNode();
    const auto patterns = ExtractPatternStrings(arguments[1]);
    result.SetValue(EvaluateNodeRelationshipExists(node, patterns));
  } catch (const std::exception &e) {
    result.SetErrorMessage(e.what());
  }
}

void Node::RelationshipTypes(mgp_list *args, mgp_graph * /*memgraph_graph*/, mgp_result *result, mgp_memory *memory) {
  const mgp::MemoryDispatcherGuard guard{memory};
  const auto arguments = mgp::List(args);
  const auto record_factory = mgp::RecordFactory(result);
  try {
    auto record = record_factory.NewRecord();
    record.Insert(std::string(kResultRelationshipTypes).c_str(), GetRelationshipTypes(arguments[0], arguments[1]));

  } catch (const std::exception &e) {
    record_factory.SetErrorMessage(e.what());
    return;
  }
}

void Node::DegreeIn(mgp_list *args, mgp_func_context * /*ctx*/, mgp_func_result *res, mgp_memory *memory) {
  const mgp::MemoryDispatcherGuard guard{memory};
  const auto arguments = mgp::List(args);
  auto result = mgp::Result(res);
  try {
    const auto node = arguments[0].ValueNode();
    const auto type = arguments[1].ValueString();
    if (type.empty()) {
      result.SetValue((int64_t)node.InDegree());
      return;
    }
    int64_t degree = 0;
    for (const auto rel : node.InRelationships()) {
      if (rel.Type() == type) {
        degree += 1;
      }
    }
    result.SetValue(degree);

  } catch (const std::exception &e) {
    result.SetErrorMessage(e.what());
    return;
  }
}

void Node::DegreeOut(mgp_list *args, mgp_func_context * /*ctx*/, mgp_func_result *res, mgp_memory *memory) {
  const mgp::MemoryDispatcherGuard guard{memory};
  const auto arguments = mgp::List(args);
  auto result = mgp::Result(res);
  try {
    const auto node = arguments[0].ValueNode();
    const auto type = arguments[1].ValueString();
    if (type.empty()) {
      result.SetValue((int64_t)node.OutDegree());
      return;
    }
    int64_t degree = 0;
    for (const auto rel : node.OutRelationships()) {
      if (rel.Type() == type) {
        degree += 1;
      }
    }
    result.SetValue(degree);

  } catch (const std::exception &e) {
    result.SetErrorMessage(e.what());
    return;
  }
}
