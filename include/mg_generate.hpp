// Copyright 2025 Memgraph Ltd.
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

#include <algorithm>
#include <chrono>
#include <memory>
#include <queue>
#include <random>
#include <set>
#include <stdexcept>

#include "mg_graph.hpp"

namespace mg_generate {

///
/// @brief Builds an undirected graph from given number of nodes and list of edges.
/// Nodes should be 0-indexed and each edge should represent an undirected connection between specified nodes.
///
///@param num_nodes Number of nodes in graph
///@param edges Edges in newly built graph
///@param graph_type Directedness of the graph
///@return std::unique_ptr<mg_graph::Graph<>> Pointer to the created graph
///
inline std::unique_ptr<mg_graph::Graph<>> BuildGraph(
    std::size_t num_nodes, const std::vector<std::pair<std::uint64_t, std::uint64_t>> &edges,
    const mg_graph::GraphType graph_type = mg_graph::GraphType::kUndirectedGraph) {
  auto G = std::make_unique<mg_graph::Graph<>>();
  for (std::size_t i = 0; i < num_nodes; ++i) G->CreateNode(i);
  for (const auto &[from, to] : edges) G->CreateEdge(from, to, graph_type);

  return G;
}

/// @brief Builds a weighted graph from a given number of nodes and a list of edges.
/// Nodes should be 0-indexed and each edge should represent an undirected connection between specified nodes.
///
///@param num_nodes Number of nodes in the graph
///@param edges Edges in the newly built graph
///@param graph_type Directedness of the graph
///@return std::unique_ptr<mg_graph::Graph<>> Pointer to the created graph
///
inline std::unique_ptr<mg_graph::Graph<>> BuildWeightedGraph(
    std::size_t num_nodes, const std::vector<std::pair<std::pair<std::uint64_t, std::uint64_t>, double>> &edges,
    const mg_graph::GraphType graph_type = mg_graph::GraphType::kUndirectedGraph) {
  auto G = std::make_unique<mg_graph::Graph<>>();
  for (std::size_t i = 0; i < num_nodes; ++i) G->CreateNode(i);
  for (const auto &[endpoints, weight] : edges) {
    auto from = endpoints.first;
    auto to = endpoints.second;
    G->CreateEdge(from, to, graph_type, std::nullopt, true, weight);
  }

  return G;
}

///
/// @brief Builds the undirected graph from a given number of nodes and a list of edges.
/// Nodes should be 0-indexed and each edge should represent an undirected connection between specified nodes.
///
///@param num_nodes Number of nodes in graph
///@param edges Edges in newly built graph
///@return std::unique_ptr<mg_graph::Graph<>> Pointer to the created graph
///
inline std::unique_ptr<mg_graph::Graph<>> BuildGraph(
    const std::vector<std::uint64_t> &nodes, const std::vector<std::pair<std::uint64_t, std::uint64_t>> &edges,
    const mg_graph::GraphType graph_type = mg_graph::GraphType::kUndirectedGraph) {
  auto G = std::make_unique<mg_graph::Graph<>>();
  for (auto node_id : nodes) G->CreateNode(node_id);
  for (const auto &[from, to] : edges) G->CreateEdge(from, to, graph_type);

  return G;
}

///
/// @brief Generates random undirected graph with a given numer of nodes and edges.
/// The generated graph is not picked out of a uniform distribution.
///
///@param num_nodes Number of nodes in graph
///@param num_edges Number of edges in graph
///@return std::unique_ptr<mg_graph::Graph<>> Pointer to the created graph
///
inline std::unique_ptr<mg_graph::Graph<>> GenRandomGraph(
    std::size_t num_nodes, std::size_t num_edges,
    const mg_graph::GraphType graph_type = mg_graph::GraphType::kUndirectedGraph) {
  using IntPair = std::pair<std::uint64_t, std::uint64_t>;

  const bool is_directed = (graph_type == mg_graph::GraphType::kDirectedGraph);
  if (num_nodes <= 1) {
    if (num_edges > 0) {
      throw std::invalid_argument("Requested number of edges exceeds maximum simple edges for node count.");
    }
    return BuildGraph(num_nodes, {}, graph_type);
  }
  const std::size_t max_directed_per_node = num_nodes - 1;
  const bool exceeds_bound =
      is_directed ? (num_edges / num_nodes > max_directed_per_node ||
                     (num_edges / num_nodes == max_directed_per_node && num_edges % num_nodes != 0))
                  : (num_nodes % 2 == 0
                         ? (num_edges / (num_nodes / 2) > max_directed_per_node ||
                            (num_edges / (num_nodes / 2) == max_directed_per_node && num_edges % (num_nodes / 2) != 0))
                         : (num_edges / num_nodes > max_directed_per_node / 2 ||
                            (num_edges / num_nodes == max_directed_per_node / 2 && num_edges % num_nodes != 0)));
  if (exceeds_bound) {
    throw std::invalid_argument("Requested number of edges exceeds maximum simple edges for node count.");
  }
  if (num_edges == 0) {
    return BuildGraph(num_nodes, {}, graph_type);
  }

  auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::mt19937 rng(seed);

  // When dense (num_edges > half of max possible simple edges), shuffle the candidate pool directly.
  const bool small_node_count = num_nodes <= (std::numeric_limits<std::size_t>::max() / num_nodes);
  const std::size_t max_edges = small_node_count
                                    ? (is_directed ? num_nodes * (num_nodes - 1) : (num_nodes * (num_nodes - 1)) / 2)
                                    : std::numeric_limits<std::size_t>::max();
  if (small_node_count && num_edges > max_edges / 2) {
    std::vector<IntPair> pool;
    pool.reserve(max_edges);
    for (std::uint64_t u = 0; u < num_nodes; ++u) {
      const std::uint64_t start_v = is_directed ? 0 : (u + 1);
      for (std::uint64_t v = start_v; v < num_nodes; ++v) {
        if (u != v) pool.emplace_back(u, v);
      }
    }
    std::shuffle(pool.begin(), pool.end(), rng);
    pool.resize(num_edges);
    return BuildGraph(num_nodes, pool, graph_type);
  }

  std::uniform_int_distribution<std::uint64_t> dist(0, num_nodes - 1);
  std::set<IntPair> rand_edges;
  for (std::size_t i = 0; i < num_edges; ++i) {
    IntPair edge{};
    do {
      const auto u = dist(rng);
      const auto v = dist(rng);
      edge = is_directed ? IntPair{u, v} : IntPair{std::minmax(u, v)};
    } while (edge.first == edge.second || rand_edges.find(edge) != rand_edges.end());
    rand_edges.insert(edge);
  }
  return BuildGraph(num_nodes, {rand_edges.begin(), rand_edges.end()}, graph_type);
}

///
/// @brief Generates a random undirected tree with a given number of nodes.
/// The generated tree is not picked out of a uniform distribution.
///
///@param num_nodes Number of nodes in graph
///@return std::unique_ptr<mg_graph::Graph<>> Pointer to the created graph
///
inline std::unique_ptr<mg_graph::Graph<>> GenRandomTree(
    std::size_t num_nodes, const mg_graph::GraphType graph_type = mg_graph::GraphType::kUndirectedGraph) {
  auto seed = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  std::mt19937 rng(seed);
  std::vector<std::pair<std::uint64_t, std::uint64_t>> edges;
  for (std::size_t i = 1; i < num_nodes; ++i) {
    std::uniform_int_distribution<std::uint64_t> dist(0, i - 1);
    edges.emplace_back(dist(rng), i);
  }
  return BuildGraph(num_nodes, edges, graph_type);
}

}  // namespace mg_generate
