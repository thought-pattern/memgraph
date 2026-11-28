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

#include <algorithm>
#include <fmt/format.h>
#include <mgp.hpp>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

constexpr const char *kProcedurePeriodicIterate = "iterate";
constexpr const char *kProcedurePeriodicDelete = "delete";
constexpr const char *kArgumentInputQuery = "input_query";
constexpr const char *kArgumentRunningQuery = "running_query";
constexpr const char *kArgumentConfig = "config";
constexpr const char *kConfigKeyBatchSize = "batch_size";
constexpr const char *kConfigKeyLabels = "labels";
constexpr const char *kConfigKeyEdgeTypes = "edge_types";

constexpr const char *kReturnSuccess = "success";
constexpr const char *kReturnNumBatches = "number_of_executed_batches";
constexpr const char *kReturnNumDeletedNodes = "number_of_deleted_nodes";
constexpr const char *kReturnNumDeletedRelationships = "number_of_deleted_relationships";
constexpr const char *kReturnInternalNumDeleted = "num_deleted";
constexpr const char *kReturnInternalNumExtraRels = "num_extra_rels";

struct DeletionInfo {
  uint64_t batch_size{0};
  std::vector<std::string> labels;
  std::vector<std::string> edge_types;
};

struct DeletionResult {
  uint64_t num_batches{0};
  uint64_t num_deleted_nodes{0};
  uint64_t num_deleted_relationships{0};
};

namespace {

std::string QuoteCypherIdentifier(std::string_view identifier) {
  std::string escaped;
  escaped.reserve(identifier.size() + 2);
  escaped.push_back('`');
  for (const char ch : identifier) {
    if (ch == '`') {
      escaped.push_back('`');
    }
    escaped.push_back(ch);
  }
  escaped.push_back('`');
  return escaped;
}

std::string SelectInternalPrefix(const mgp::ExecutionHeaders &headers) {
  std::string prefix = "__mg_periodic_";
  auto collides = [&headers](std::string_view candidate) {
    for (size_t i = 0; i < headers.Size(); ++i) {
      if (std::string_view(headers[i]).starts_with(candidate)) {
        return true;
      }
    }
    return false;
  };
  while (collides(prefix)) {
    prefix.push_back('_');
  }
  return prefix;
}

std::string Join(const std::vector<std::string> &strings, const std::string &delimiter) {
  if (strings.empty()) {
    return "";
  }

  size_t joined_strings_size = 0;
  for (const auto &string : strings) {
    joined_strings_size += string.size();
  }

  std::string joined_strings;
  joined_strings.reserve(joined_strings_size + (delimiter.size() * (strings.size() - 1)));

  joined_strings += strings[0];
  for (size_t i = 1; i < strings.size(); i++) {
    joined_strings += delimiter + strings[i];
  }

  return joined_strings;
}

std::vector<bool> DetectGraphEntityColumns(const mgp::ExecutionHeaders &headers, const mgp::List &batch) {
  std::vector<bool> has_entity(headers.Size(), false);
  for (size_t row = 0; row < batch.Size(); ++row) {
    const mgp::List row_list = batch[row].ValueList();
    for (size_t col = 0; col < headers.Size(); ++col) {
      if (row_list[col].IsNode() || row_list[col].IsRelationship()) {
        has_entity[col] = true;
      }
    }
  }
  return has_entity;
}

std::string ConstructQueryPrefix(const mgp::ExecutionHeaders &headers, const std::vector<bool> &has_entity,
                                 const std::string &prefix) {
  if (headers.Size() == 0) {
    return {};
  }

  const auto batch_param = QuoteCypherIdentifier(prefix + "batch");
  const auto row_var = QuoteCypherIdentifier(prefix + "row");
  const auto unwind_batch = fmt::format("UNWIND ${} AS {}", batch_param, row_var);

  std::vector<std::string> initial_with;
  std::vector<std::string> optional_matches;
  std::vector<std::string> resolved_with;
  initial_with.reserve(headers.Size());
  resolved_with.reserve(headers.Size());

  const bool any_entity = std::ranges::any_of(has_entity, [](bool v) { return v; });
  for (size_t i = 0; i < headers.Size(); ++i) {
    const auto quoted_col = QuoteCypherIdentifier(headers[i]);
    if (!has_entity[i]) {
      initial_with.emplace_back(fmt::format("{0}.{1} AS {1}", row_var, quoted_col));
      if (any_entity) {
        resolved_with.emplace_back(quoted_col);
      }
      continue;
    }

    const auto cell_var = QuoteCypherIdentifier(fmt::format("{}c_{}", prefix, i));
    const auto node_var = QuoteCypherIdentifier(fmt::format("{}n_{}", prefix, i));
    const auto rel_var = QuoteCypherIdentifier(fmt::format("{}r_{}", prefix, i));
    initial_with.emplace_back(fmt::format("{}.{} AS {}", row_var, quoted_col, cell_var));
    optional_matches.emplace_back(
        fmt::format("OPTIONAL MATCH ({0}) WHERE {1}.k = 1 AND ID({0}) = {1}.id", node_var, cell_var));
    optional_matches.emplace_back(
        fmt::format("OPTIONAL MATCH ()-[{0}]->() WHERE {1}.k = 2 AND ID({0}) = {1}.id", rel_var, cell_var));
    resolved_with.emplace_back(fmt::format(
        "CASE {0}.k WHEN 1 THEN {1} WHEN 2 THEN {2} ELSE {0}.v END AS {3}", cell_var, node_var, rel_var, quoted_col));
  }

  if (!any_entity) {
    return fmt::format("{} WITH {}", unwind_batch, Join(initial_with, ", "));
  }
  return fmt::format(
      "{} WITH {} {} WITH {}", unwind_batch, Join(initial_with, ", "), Join(optional_matches, " "), Join(resolved_with, ", "));
}

mgp::Map ConstructQueryParams(const mgp::ExecutionHeaders &headers, const mgp::List &batch,
                              const std::vector<bool> &has_entity, const std::string &prefix) {
  mgp::List list_value(batch.Size());
  const auto param_row_size = headers.Size();

  for (size_t row = 0; row < batch.Size(); row++) {
    mgp::Map constructed_row;
    mgp::List row_list = batch[row].ValueList();

    for (size_t i = 0; i < param_row_size; i++) {
      if (!has_entity[i]) {
        constructed_row.Insert(headers[i], row_list[i]);
        continue;
      }
      mgp::Map tagged_cell;
      if (row_list[i].IsNode()) {
        tagged_cell.Insert("k", mgp::Value(int64_t{1}));
        tagged_cell.Insert("id", mgp::Value(row_list[i].ValueNode().Id().AsInt()));
      } else if (row_list[i].IsRelationship()) {
        tagged_cell.Insert("k", mgp::Value(int64_t{2}));
        tagged_cell.Insert("id", mgp::Value(row_list[i].ValueRelationship().Id().AsInt()));
      } else {
        tagged_cell.Insert("k", mgp::Value(int64_t{0}));
        tagged_cell.Insert("v", row_list[i]);
      }
      constructed_row.Insert(headers[i], mgp::Value(std::move(tagged_cell)));
    }

    list_value.Append(mgp::Value(std::move(constructed_row)));
  }

  mgp::Map params;
  params.Insert(prefix + "batch", mgp::Value(std::move(list_value)));

  return params;
}

std::string ConstructFinalQuery(const std::string &running_query, const std::string &prefix_query) {
  if (prefix_query.empty()) {
    return running_query;
  }
  return fmt::format("{} {}", prefix_query, running_query);
}

void ExecuteRunningQuery(const mgp::QueryExecution &query_execution, std::string running_query,
                         const mgp::ExecutionHeaders &headers, const mgp::List &batch) {
  if (batch.Empty()) {
    return;
  }

  const auto prefix = SelectInternalPrefix(headers);
  const auto has_entity = DetectGraphEntityColumns(headers, batch);
  auto prefix_query = ConstructQueryPrefix(headers, has_entity, prefix);
  auto final_query = ConstructFinalQuery(running_query, prefix_query);

  auto query_params = ConstructQueryParams(headers, batch, has_entity, prefix);

  auto execution_result = query_execution.ExecuteQuery(final_query, query_params);

  while (execution_result.PullOne()) {
  }
}

void ValidateBatchSizeFromConfig(const mgp::Map &config) {
  auto batch_size_key = std::string(kConfigKeyBatchSize);

  if (!config.KeyExists(batch_size_key)) {
    throw std::runtime_error(fmt::format("Configuration parameter {} is not set.", kConfigKeyBatchSize));
  }

  auto batch_size_value = config.At(batch_size_key);
  if (!batch_size_value.IsInt()) {
    throw std::runtime_error("Batch size needs to be an integer!");
  }

  if (batch_size_value.ValueInt() <= 0) {
    throw std::runtime_error("Batch size can't be a non-negative integer!");
  }
}

void ValidateDeletionConfigEntities(const mgp::Map &config, std::string config_key) {
  auto key = std::string_view(config_key);
  if (!config.KeyExists(key)) {
    return;
  }

  auto value = config.At(key);
  if (!value.IsString() && !value.IsList()) {
    throw std::runtime_error(fmt::format("Invalid config for config parameter {}!", config_key));
  }

  if (value.IsString()) {
    if (value.ValueString().empty()) {
      throw std::runtime_error(fmt::format("Config parameter {} must not be an empty string!", config_key));
    }
    return;
  }

  auto list_value = value.ValueList();
  for (auto elem : list_value) {
    if (!elem.IsString() || elem.ValueString().empty()) {
      throw std::runtime_error(fmt::format("Invalid config for config parameter {}!", config_key));
    }
  }
}

void ValidateDeletionConfig(const mgp::Map &config) {
  auto labels_key = std::string(kConfigKeyLabels);
  auto edge_types_key = std::string(kConfigKeyEdgeTypes);

  for (const auto &item : config) {
    if (item.key != kConfigKeyBatchSize && item.key != kConfigKeyLabels && item.key != kConfigKeyEdgeTypes) {
      throw std::runtime_error(fmt::format(
          "Unknown configuration parameter '{}' for periodic.delete; allowed keys are batch_size, labels, edge_types.",
          item.key));
    }
  }

  ValidateBatchSizeFromConfig(config);
  ValidateDeletionConfigEntities(config, labels_key);
  ValidateDeletionConfigEntities(config, edge_types_key);
}

void EmplaceFromConfig(const mgp::Map &config, std::vector<std::string> &vec, std::string &config_key) {
  auto key = std::string_view(config_key);
  if (!config.KeyExists(key)) {
    return;
  }

  auto value = config.At(key);
  if (value.IsString()) {
    vec.emplace_back(value.ValueString());
  } else if (value.IsList()) {
    auto list_value = value.ValueList();
    for (const auto elem : list_value) {
      vec.emplace_back(elem.ValueString());
    }
  }
}

DeletionInfo GetDeletionInfo(const mgp::Map &config) {
  std::vector<std::string> labels;
  std::vector<std::string> edge_types;

  ValidateDeletionConfig(config);

  auto batch_size_key = std::string(kConfigKeyBatchSize);
  auto labels_key = std::string(kConfigKeyLabels);
  auto edge_types_key = std::string(kConfigKeyEdgeTypes);

  auto batch_size = config.At(batch_size_key).ValueInt();

  EmplaceFromConfig(config, labels, labels_key);
  EmplaceFromConfig(config, edge_types, edge_types_key);

  return {.batch_size = static_cast<uint64_t>(batch_size),
          .labels = std::move(labels),
          .edge_types = std::move(edge_types)};
}

void ExecutePeriodicDelete(const mgp::QueryExecution &query_execution, const DeletionInfo &deletion_info,
                           DeletionResult &deletion_result) {
  auto delete_all = deletion_info.edge_types.empty() && deletion_info.labels.empty();
  auto delete_nodes = delete_all || !deletion_info.labels.empty();
  auto delete_edges = delete_all || !deletion_info.labels.empty() || !deletion_info.edge_types.empty();

  std::vector<std::string> quoted_labels;
  quoted_labels.reserve(deletion_info.labels.size());
  for (const auto &lbl : deletion_info.labels) {
    quoted_labels.push_back(QuoteCypherIdentifier(lbl));
  }
  std::vector<std::string> quoted_edge_types;
  quoted_edge_types.reserve(deletion_info.edge_types.size());
  for (const auto &et : deletion_info.edge_types) {
    quoted_edge_types.push_back(QuoteCypherIdentifier(et));
  }

  auto labels_formatted = quoted_labels.empty() ? "" : fmt::format(":{}", Join(quoted_labels, ":"));
  auto edge_types_formatted = quoted_edge_types.empty() ? "" : fmt::format(":{}", Join(quoted_edge_types, "|"));

  auto relationships_deletion_query =
      fmt::format("MATCH (n{})-[r{}]-(m) WITH DISTINCT r LIMIT {} DELETE r RETURN count(r) AS num_deleted",
                  labels_formatted,
                  edge_types_formatted,
                  deletion_info.batch_size);
  auto nodes_deletion_query = fmt::format(
      "MATCH (n{}) WITH DISTINCT n LIMIT {} OPTIONAL MATCH (n)-[r]-() "
      "WITH collect(DISTINCT n) AS __mg_nodes_to_delete, count(DISTINCT r) AS num_extra_rels "
      "FOREACH (__mg_node IN __mg_nodes_to_delete | DETACH DELETE __mg_node) "
      "RETURN size(__mg_nodes_to_delete) AS num_deleted, num_extra_rels",
      labels_formatted,
      deletion_info.batch_size);

  if (delete_edges) {
    while (true) {
      auto execution_result = query_execution.ExecuteQuery(relationships_deletion_query);

      auto result = execution_result.PullOne();
      if (!result || (*result).Size() == 0) {
        throw std::runtime_error("No result received from periodic delete!");
      }

      while (execution_result.PullOne()) {
      };

      const uint64_t num_deleted = static_cast<uint64_t>((*result).At(kReturnInternalNumDeleted).ValueInt());
      deletion_result.num_batches++;
      deletion_result.num_deleted_relationships += num_deleted;
      if (num_deleted < deletion_info.batch_size) {
        break;
      }
    }
  }

  if (delete_nodes) {
    while (true) {
      auto execution_result = query_execution.ExecuteQuery(nodes_deletion_query);

      auto result = execution_result.PullOne();
      if (!result || (*result).Size() == 0) {
        throw std::runtime_error("No result received from periodic delete!");
      }

      while (execution_result.PullOne()) {
      };

      const uint64_t num_deleted = static_cast<uint64_t>((*result).At(kReturnInternalNumDeleted).ValueInt());
      uint64_t num_extra_rels = 0;
      if ((*result).KeyExists(kReturnInternalNumExtraRels)) {
        num_extra_rels = static_cast<uint64_t>((*result).At(kReturnInternalNumExtraRels).ValueInt());
      }
      deletion_result.num_batches++;
      deletion_result.num_deleted_nodes += num_deleted;
      deletion_result.num_deleted_relationships += num_extra_rels;
      if (num_deleted < deletion_info.batch_size) {
        break;
      }
    }
  }
}

void PeriodicDelete(mgp_list *args, mgp_graph *memgraph_graph, mgp_result *result, mgp_memory *memory) {
  const mgp::MemoryDispatcherGuard guard{memory};
  const auto arguments = mgp::List(args);

  const auto record_factory = mgp::RecordFactory(result);
  auto record = record_factory.NewRecord();

  const auto config = arguments[0].ValueMap();

  DeletionResult deletion_result;

  try {
    auto query_execution = mgp::QueryExecution(memgraph_graph);

    auto deletion_info = GetDeletionInfo(config);

    ExecutePeriodicDelete(query_execution, deletion_info, deletion_result);

    record.Insert(kReturnSuccess, true);
    record.Insert(kReturnNumBatches, static_cast<int64_t>(deletion_result.num_batches));
    record.Insert(kReturnNumDeletedNodes, static_cast<int64_t>(deletion_result.num_deleted_nodes));
    record.Insert(kReturnNumDeletedRelationships, static_cast<int64_t>(deletion_result.num_deleted_relationships));
  } catch (const std::exception &e) {
    record_factory.SetErrorMessage(e.what());
    record.Insert(kReturnSuccess, false);
    record.Insert(kReturnNumBatches, static_cast<int64_t>(deletion_result.num_batches));
    record.Insert(kReturnNumDeletedNodes, static_cast<int64_t>(deletion_result.num_deleted_nodes));
    record.Insert(kReturnNumDeletedRelationships, static_cast<int64_t>(deletion_result.num_deleted_relationships));
  }
}

void PeriodicIterate(mgp_list *args, mgp_graph *memgraph_graph, mgp_result *result, mgp_memory *memory) {
  const mgp::MemoryDispatcherGuard guard{memory};
  const auto arguments = mgp::List(args);

  auto num_of_executed_batches = 0;
  const auto record_factory = mgp::RecordFactory(result);
  auto record = record_factory.NewRecord();

  const auto input_query = std::string(arguments[0].ValueString());
  const auto running_query = std::string(arguments[1].ValueString());
  const auto config = arguments[2].ValueMap();

  try {
    ValidateBatchSizeFromConfig(config);
    const auto batch_size = config.At(kConfigKeyBatchSize).ValueInt();

    auto input_query_execution = mgp::QueryExecution(memgraph_graph);
    auto execution_result = input_query_execution.ExecuteQuery(input_query);

    auto headers = execution_result.Headers();

    mgp::List batch(batch_size);
    int rows = 0;
    while (const auto maybe_result = execution_result.PullOne()) {
      if ((*maybe_result).Size() == 0) {
        break;
      }

      const auto &result = *maybe_result;
      mgp::List row(result.Size());
      for (const auto &header : headers) {
        row.Append(result.At(header));
      }
      batch.Append(mgp::Value(std::move(row)));

      rows++;

      if (rows == batch_size) {
        auto iterative_query_execution = mgp::QueryExecution(memgraph_graph);
        ExecuteRunningQuery(iterative_query_execution, running_query, headers, batch);
        num_of_executed_batches++;
        rows = 0;
        batch = mgp::List(batch_size);
      }
    }

    if (!batch.Empty()) {
      auto iterative_query_execution = mgp::QueryExecution(memgraph_graph);
      ExecuteRunningQuery(iterative_query_execution, running_query, headers, batch);
      num_of_executed_batches++;
    }

    record.Insert(kReturnSuccess, true);
    record.Insert(kReturnNumBatches, static_cast<int64_t>(num_of_executed_batches));
  } catch (const std::exception &e) {
    record_factory.SetErrorMessage(e.what());
    record.Insert(kReturnSuccess, false);
    record.Insert(kReturnNumBatches, static_cast<int64_t>(num_of_executed_batches));
  }
}

}  // namespace

extern "C" int mgp_init_module(struct mgp_module *module, struct mgp_memory *memory) {
  try {
    const mgp::MemoryDispatcherGuard guard{memory};
    mgp::AddProcedure(PeriodicIterate,
                      kProcedurePeriodicIterate,
                      mgp::ProcedureType::Read,
                      {mgp::Parameter(kArgumentInputQuery, mgp::Type::String),
                       mgp::Parameter(kArgumentRunningQuery, mgp::Type::String),
                       mgp::Parameter(kArgumentConfig, mgp::Type::Map)},
                      {mgp::Return(kReturnSuccess, mgp::Type::Bool), mgp::Return(kReturnNumBatches, mgp::Type::Int)},
                      module,
                      memory);

    mgp::AddProcedure(PeriodicDelete,
                      kProcedurePeriodicDelete,
                      mgp::ProcedureType::Read,
                      {mgp::Parameter(kArgumentConfig, mgp::Type::Map)},
                      {mgp::Return(kReturnSuccess, mgp::Type::Bool),
                       mgp::Return(kReturnNumBatches, mgp::Type::Int),
                       mgp::Return(kReturnNumDeletedNodes, mgp::Type::Int),
                       mgp::Return(kReturnNumDeletedRelationships, mgp::Type::Int)},
                      module,
                      memory);
  } catch (const std::exception &e) {
    return 1;
  }
  return 0;
}

extern "C" int mgp_shutdown_module() { return 0; }
