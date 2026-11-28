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
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <mgp.hpp>

namespace VectorSearch {
static constexpr std::string_view kProcedureSearch = "search";
static constexpr std::string_view kProcedureSearchEdges = "search_edges";
static constexpr std::string_view kParameterIndexName = "index_name";
static constexpr std::string_view kParameterResultSetSize = "result_set_size";
static constexpr std::string_view kParameterQueryVector = "query_vector";
static constexpr std::string_view kReturnNode = "node";
static constexpr std::string_view kReturnEdge = "edge";
static constexpr std::string_view kReturnDistance = "distance";
static constexpr std::string_view kReturnSimilarity = "similarity";

static constexpr std::string_view kProcedureShowIndexInfo = "show_index_info";
static constexpr std::string_view kReturnIndexName = "index_name";
static constexpr std::string_view kReturnLabel = "label";
static constexpr std::string_view kReturnProperty = "property";
static constexpr std::string_view kMetric = "metric";
static constexpr std::string_view kReturnDimension = "dimension";
static constexpr std::string_view kReturnCapacity = "capacity";
static constexpr std::string_view kReturnSize = "size";
static constexpr std::string_view kReturnScalarKind = "scalar_kind";
static constexpr std::string_view kReturnIndexType = "index_type";

static constexpr std::string_view kProcedureCosineSimilarity = "cosine_similarity";
static constexpr std::string_view kParameterVector1 = "vector1";
static constexpr std::string_view kParameterVector2 = "vector2";

void Search(mgp_list *args, mgp_graph *memgraph_graph, mgp_result *result, mgp_memory *memory);
void SearchEdges(mgp_list *args, mgp_graph *memgraph_graph, mgp_result *result, mgp_memory *memory);
void ShowIndexInfo(mgp_list *args, mgp_graph *memgraph_graph, mgp_result *result, mgp_memory *memory);
void CosineSimilarityFunction(mgp_list *args, mgp_func_context *ctx, mgp_func_result *res, mgp_memory *memory);
}  // namespace VectorSearch

namespace {
void ReportProcedureError(const mgp::RecordFactory &record_factory, const char *message) noexcept {
  try {
    record_factory.SetErrorMessage(message);
  } catch (...) {
  }
}

void ReportFunctionError(mgp_func_result *res, const char *message, mgp_memory *memory) noexcept {
  try {
    mgp::func_result_set_error_msg(res, message, memory);
  } catch (...) {
  }
}
}  // namespace

void VectorSearch::Search(mgp_list *args, mgp_graph *memgraph_graph, mgp_result *result, mgp_memory *memory) {
  mgp::MemoryDispatcherGuard guard{memory};
  const auto record_factory = mgp::RecordFactory(result);

  try {
    auto arguments = mgp::List(args);
    const auto index_name = arguments[0].ValueString();
    const auto result_set_size = arguments[1].ValueInt();
    if (result_set_size < 0 || result_set_size > std::numeric_limits<int>::max()) {
      throw std::invalid_argument("result_set_size must be between 0 and INT_MAX.");
    }
    auto query_vector = arguments[2].ValueList();

    auto results = mgp::SearchVectorIndex(memgraph_graph, index_name, query_vector, static_cast<int>(result_set_size));

    for (const auto &result : results) {
      auto record = record_factory.NewRecord();

      auto result_list = result.ValueList();
      record.Insert(VectorSearch::kReturnNode.data(), result_list[0].ValueNode());
      record.Insert(VectorSearch::kReturnDistance.data(), result_list[1].ValueDouble());
      record.Insert(VectorSearch::kReturnSimilarity.data(), result_list[2].ValueDouble());
    }

  } catch (const std::exception &e) {
    ReportProcedureError(record_factory, e.what());
  } catch (...) {
    ReportProcedureError(record_factory, "Unknown error occurred in vector_search.search.");
  }
}

void VectorSearch::SearchEdges(mgp_list *args, mgp_graph *memgraph_graph, mgp_result *result, mgp_memory *memory) {
  mgp::MemoryDispatcherGuard guard{memory};
  const auto record_factory = mgp::RecordFactory(result);

  try {
    auto arguments = mgp::List(args);
    const auto index_name = arguments[0].ValueString();
    const auto result_set_size = arguments[1].ValueInt();
    if (result_set_size < 0 || result_set_size > std::numeric_limits<int>::max()) {
      throw std::invalid_argument("result_set_size must be between 0 and INT_MAX.");
    }
    auto query_vector = arguments[2].ValueList();

    auto results =
        mgp::SearchVectorIndexOnEdges(memgraph_graph, index_name, query_vector, static_cast<int>(result_set_size));

    for (const auto &result : results) {
      auto record = record_factory.NewRecord();

      auto result_list = result.ValueList();
      record.Insert(VectorSearch::kReturnEdge.data(), result_list[0].ValueRelationship());
      record.Insert(VectorSearch::kReturnDistance.data(), result_list[1].ValueDouble());
      record.Insert(VectorSearch::kReturnSimilarity.data(), result_list[2].ValueDouble());
    }

  } catch (const std::exception &e) {
    ReportProcedureError(record_factory, e.what());
  } catch (...) {
    ReportProcedureError(record_factory, "Unknown error occurred in vector_search.search_edges.");
  }
}

void VectorSearch::ShowIndexInfo(mgp_list *args, mgp_graph *memgraph_graph, mgp_result *result, mgp_memory *memory) {
  mgp::MemoryDispatcherGuard guard{memory};
  const auto record_factory = mgp::RecordFactory(result);

  try {
    auto arguments = mgp::List(args);
    (void)arguments;
    const auto index_info = mgp::GetVectorIndexInfo(memgraph_graph);

    for (const auto &info : index_info) {
      auto record = record_factory.NewRecord();
      auto info_list = info.ValueList();
      record.Insert(VectorSearch::kReturnIndexName.data(), info_list[0].ValueString());
      record.Insert(VectorSearch::kReturnLabel.data(), info_list[1].ValueString());
      record.Insert(VectorSearch::kReturnProperty.data(), info_list[2].ValueString());
      record.Insert(VectorSearch::kMetric.data(), info_list[3].ValueString());
      record.Insert(VectorSearch::kReturnDimension.data(), info_list[4].ValueInt());
      record.Insert(VectorSearch::kReturnCapacity.data(), info_list[5].ValueInt());
      record.Insert(VectorSearch::kReturnSize.data(), info_list[6].ValueInt());
      record.Insert(VectorSearch::kReturnScalarKind.data(), info_list[7].ValueString());
      record.Insert(VectorSearch::kReturnIndexType.data(), info_list[8].ValueString());
    }
  } catch (const std::exception &e) {
    ReportProcedureError(record_factory, e.what());
  } catch (...) {
    ReportProcedureError(record_factory, "Unknown error occurred in vector_search.show_index_info.");
  }
}

void VectorSearch::CosineSimilarityFunction(mgp_list *args, mgp_func_context * /*ctx*/, mgp_func_result *res,
                                            mgp_memory *memory) {
  mgp::MemoryDispatcherGuard guard{memory};
  try {
    const auto vector1 = mgp::Value(mgp::ref_type, mgp::list_at(args, 0)).ValueList();
    const auto vector2 = mgp::Value(mgp::ref_type, mgp::list_at(args, 1)).ValueList();

    if (vector1.Size() == 0 || vector1.Size() != vector2.Size()) {
      throw std::invalid_argument("Vectors must be non-empty and have the same dimension");
    }

    const auto dim = vector1.Size();
    std::vector<double> vals1(dim);
    std::vector<double> vals2(dim);
    auto max_abs1 = 0.0;
    auto max_abs2 = 0.0;
    auto get_numeric_value = [](const mgp::Value &val) {
      double num = 0.0;
      if (val.IsDouble()) {
        num = val.ValueDouble();
      } else if (val.IsInt()) {
        num = static_cast<double>(val.ValueInt());
      } else {
        throw std::invalid_argument("Vector elements must be numeric (int or double)");
      }
      if (!std::isfinite(num)) {
        throw std::invalid_argument("Vector elements must be finite numeric values");
      }
      return num;
    };

    for (auto i = 0; i < dim; ++i) {
      vals1[i] = get_numeric_value(vector1[i]);
      vals2[i] = get_numeric_value(vector2[i]);
      max_abs1 = std::max(max_abs1, std::abs(vals1[i]));
      max_abs2 = std::max(max_abs2, std::abs(vals2[i]));
    }
    if (max_abs1 == 0.0 || max_abs2 == 0.0) [[unlikely]] {
      throw std::invalid_argument("Cannot calculate cosine similarity for zero vectors");
    }

    auto dot_product = 0.0;
    auto magnitude1 = 0.0;
    auto magnitude2 = 0.0;
    for (auto i = 0; i < dim; ++i) {
      const auto u1 = vals1[i] / max_abs1;
      const auto u2 = vals2[i] / max_abs2;
      dot_product += u1 * u2;
      magnitude1 += u1 * u1;
      magnitude2 += u2 * u2;
    }
    magnitude1 = std::sqrt(magnitude1);
    magnitude2 = std::sqrt(magnitude2);
    if (magnitude1 == 0.0 || magnitude2 == 0.0) [[unlikely]] {
      throw std::invalid_argument("Cannot calculate cosine similarity for zero vectors");
    }

    const auto cosine_similarity = std::clamp(dot_product / (magnitude1 * magnitude2), -1.0, 1.0);
    auto result = mgp::Result(res);
    result.SetValue(cosine_similarity);
  } catch (const std::exception &e) {
    ReportFunctionError(res, e.what(), memory);
  } catch (...) {
    ReportFunctionError(res, "Unknown error occurred in vector_search.cosine_similarity.", memory);
  }
}

extern "C" int mgp_init_module(struct mgp_module *module, struct mgp_memory *memory) {
  try {
    mgp::MemoryDispatcherGuard guard{memory};
    AddProcedure(VectorSearch::Search,
                 VectorSearch::kProcedureSearch,
                 mgp::ProcedureType::Read,
                 {
                     mgp::Parameter(VectorSearch::kParameterIndexName, mgp::Type::String),
                     mgp::Parameter(VectorSearch::kParameterResultSetSize, mgp::Type::Int),
                     mgp::Parameter(VectorSearch::kParameterQueryVector, {mgp::Type::List, mgp::Type::Any}),
                 },
                 {
                     mgp::Return(VectorSearch::kReturnNode, mgp::Type::Node),
                     mgp::Return(VectorSearch::kReturnDistance, mgp::Type::Double),
                     mgp::Return(VectorSearch::kReturnSimilarity, mgp::Type::Double),
                 },
                 module,
                 memory);

    AddProcedure(VectorSearch::ShowIndexInfo,
                 VectorSearch::kProcedureShowIndexInfo,
                 mgp::ProcedureType::Read,
                 {},
                 {
                     mgp::Return(VectorSearch::kReturnIndexName, mgp::Type::String),
                     mgp::Return(VectorSearch::kReturnLabel, mgp::Type::String),
                     mgp::Return(VectorSearch::kReturnProperty, mgp::Type::String),
                     mgp::Return(VectorSearch::kMetric, mgp::Type::String),
                     mgp::Return(VectorSearch::kReturnDimension, mgp::Type::Int),
                     mgp::Return(VectorSearch::kReturnCapacity, mgp::Type::Int),
                     mgp::Return(VectorSearch::kReturnSize, mgp::Type::Int),
                     mgp::Return(VectorSearch::kReturnScalarKind, mgp::Type::String),
                     mgp::Return(VectorSearch::kReturnIndexType, mgp::Type::String),
                 },
                 module,
                 memory);

    AddProcedure(VectorSearch::SearchEdges,
                 VectorSearch::kProcedureSearchEdges,
                 mgp::ProcedureType::Read,
                 {
                     mgp::Parameter(VectorSearch::kParameterIndexName, mgp::Type::String),
                     mgp::Parameter(VectorSearch::kParameterResultSetSize, mgp::Type::Int),
                     mgp::Parameter(VectorSearch::kParameterQueryVector, {mgp::Type::List, mgp::Type::Any}),
                 },
                 {
                     mgp::Return(VectorSearch::kReturnEdge, mgp::Type::Relationship),
                     mgp::Return(VectorSearch::kReturnDistance, mgp::Type::Double),
                     mgp::Return(VectorSearch::kReturnSimilarity, mgp::Type::Double),
                 },
                 module,
                 memory);

    mgp::AddFunction(VectorSearch::CosineSimilarityFunction,
                     VectorSearch::kProcedureCosineSimilarity,
                     {
                         mgp::Parameter(VectorSearch::kParameterVector1, {mgp::Type::List, mgp::Type::Any}),
                         mgp::Parameter(VectorSearch::kParameterVector2, {mgp::Type::List, mgp::Type::Any}),
                     },
                     module,
                     memory);
  } catch (const std::exception &e) {
    std::cerr << "Error while initializing query module: " << e.what() << '\n';
    return 1;
  } catch (...) {
    return 1;
  }

  return 0;
}

extern "C" int mgp_shutdown_module() { return 0; }
