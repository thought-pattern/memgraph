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

/// @file graph.hpp
/// This file contains the graph definitions.

#pragma once

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "data_structures/graph_data.hpp"
#include "data_structures/graph_view.hpp"
#include "mg_exceptions.hpp"

namespace mg_graph {

enum class GraphType : std::uint8_t { kDirectedGraph, kUndirectedGraph };

/// Graph representation.
template <typename TSize = std::uint64_t>
class Graph : public GraphView<TSize> {
  using typename GraphView<TSize>::TNode;
  using typename GraphView<TSize>::TEdge;
  using typename GraphView<TSize>::TNeighbour;

  static_assert(std::is_unsigned_v<TSize>,
                "mg_graph::Graph expects the type to be an unsigned integer type\n"
                "only (uint8_t, uint16_t, uint32_t, or uint64_t).");

 public:
  /// Create object Graph
  explicit Graph() = default;

  /// Gets all graph nodes.
  ///
  /// @return Vector of nodes
  const std::vector<TNode> &Nodes() const override { return nodes_; }

  /// Gets all graph edges.
  ///
  /// This method always returns a complete list of edges. If some edges are
  /// deleted with EraseEdge method, this method will return also deleted edges.
  /// Deleted edges will have invalid edge id. Method CheckIfEdgeValid is used
  /// for checking the edge. Recommendation is to use EraseEdge method only in
  /// test cases.
  /// @return Vector of edges.
  const std::vector<TEdge> &Edges() const override { return edges_; }

  /// Gets the edges between two neighbour nodes.
  ///
  /// @param[in] first node id
  /// @param[in] second node id
  ///
  /// @return     Iterator range
  std::vector<TSize> GetEdgesBetweenNodes(TSize first, TSize second) const override {
    std::vector<TSize> edge_ids;
    const auto [range_start, range_end] = nodes_to_edge_.equal_range(std::minmax(first, second));
    edge_ids.reserve(std::distance(range_start, range_end));
    for (auto it = range_start; it != range_end; ++it) {
      if (IsEdgeValid(it->second)) {
        edge_ids.push_back(it->second);
      }
    }
    return edge_ids;
  }

  /// Gets all incident edges ids.
  ///
  /// @return all incident edges
  const std::vector<TSize> &IncidentEdges(TSize node_id) const override {
    if (node_id >= nodes_.size()) {
      throw mg_exception::InvalidIDException();
    }

    return adj_list_[node_id];
  }

  /// Gets neighbour nodes.
  ///
  /// @param[in] node_id target node id
  ///
  /// @return vector of neighbours
  const std::vector<TNeighbour> &Neighbours(TSize node_id) const override {
    if (node_id >= nodes_.size()) {
      throw mg_exception::InvalidIDException();
    }

    return neighbours_[node_id];
  }

  /// Gets out-neighbour nodes.
  ///
  /// @param[in] node_id target node id
  ///
  /// @return vector of neighbours
  const std::vector<TNeighbour> &OutNeighbours(TSize node_id) const override { return Neighbours(node_id); }

  /// Gets in-neighbour nodes.
  ///
  /// @param[in] node_id target node id
  ///
  /// @return vector of neighbours
  const std::vector<TNeighbour> &InNeighbours(TSize node_id) const override {
    if (node_id >= nodes_.size()) {
      throw mg_exception::InvalidIDException();
    }

    return in_neighbours_[node_id];
  }

  /// Gets node with node id.
  ///
  /// @param[in] node_id node id
  ///
  /// @return target Node struct
  const TNode &GetNode(TSize node_id) const override {
    if (node_id >= nodes_.size()) {
      throw mg_exception::InvalidIDException();
    }

    return nodes_[node_id];
  }

  /// Gets Edge with edge id.
  ///
  /// @param[in] edge_id edge id
  ///
  /// @return Edge struct
  const TEdge &GetEdge(TSize edge_id) const override {
    if (edge_id >= edges_.size()) {
      throw mg_exception::InvalidIDException();
    }
    return edges_[edge_id];
  }

  /// Gets edge weight by edge ID.
  ///
  /// @param[in] edge_id edge ID
  ///
  /// @return double weight
  double GetWeight(TSize edge_id) const override {
    if (!IsEdgeValid(edge_id) || edge_id >= weights_.size()) {
      throw mg_exception::InvalidIDException();
    }
    return weights_[edge_id];
  }

  ///
  ///@brief Return true if graph has weights on edges.
  ///
  bool IsWeighted() const override { return has_weights_; }

  /// Creates a node.
  ///
  /// @return     Created node id
  std::optional<TSize> CreateNode(std::uint64_t memgraph_id) {
    if (memgraph_to_inner_id_.find(memgraph_id) != memgraph_to_inner_id_.end()) {
      return std::nullopt;
    }
    if (nodes_.size() >= std::numeric_limits<TSize>::max()) {
      throw mg_exception::InvalidIDException();
    }

    const auto id = static_cast<TSize>(nodes_.size());
    nodes_.push_back(TNode{id});
    adj_list_.emplace_back();
    neighbours_.emplace_back();
    in_neighbours_.emplace_back();

    inner_to_memgraph_id_.emplace(id, memgraph_id);
    memgraph_to_inner_id_.emplace(memgraph_id, id);
    return id;
  }

  /// Creates a directed/undirected edge (per graph type) in the GraphView.
  /// Edges are defined by three Memgraph IDs: (source_node, destination_node, edge). If no edge ID is given, the
  /// assumed value is (M + 1) for a GraphView with M edges.
  ///
  /// Throws an exception if the endpoints are not in the graph.
  ///
  ///@param memgraph_id_from -- source node’s Memgraph ID
  ///@param memgraph_id_to -- destination node’s Memgraph ID
  ///@param graph_type -- graph type (directed or undirected)
  ///@param memgraph_edge_id -- edge’s Memgraph ID
  ///@param weighted -- whether the graph is weighted
  ///@param weight -- edge weight
  ///
  /// @return new edge’s inner ID
  ///
  std::optional<TSize> CreateEdge(std::uint64_t memgraph_id_from, std::uint64_t memgraph_id_to,
                                  const GraphType graph_type = GraphType::kDirectedGraph,
                                  std::optional<std::uint64_t> memgraph_edge_id = std::nullopt, bool weighted = false,
                                  double weight = 0.0) {
    std::uint64_t edge_id = (!memgraph_edge_id) ? edges_.size() : *memgraph_edge_id;

    if (memgraph_to_inner_edge_id_.find(edge_id) != memgraph_to_inner_edge_id_.end()) {
      return std::nullopt;
    }
    if (edges_.size() >= k_deleted_edge_id_) {
      throw mg_exception::InvalidIDException();
    }

    auto fromOpt = GetInnerNodeIdOpt(memgraph_id_from);
    auto toOpt = GetInnerNodeIdOpt(memgraph_id_to);
    if (!fromOpt || !toOpt) {
      return std::nullopt;
    }

    const TSize from = *fromOpt;
    const TSize to = *toOpt;

    const auto id = static_cast<TSize>(edges_.size());
    inner_to_memgraph_edge_id_.emplace(id, edge_id);
    memgraph_to_inner_edge_id_.emplace(edge_id, id);

    edges_.push_back(TEdge{id, from, to});
    adj_list_[from].push_back(id);
    neighbours_[from].emplace_back(to, id);
    nodes_to_edge_.insert({std::minmax(from, to), id});
    if (weighted) {
      has_weights_ = true;
    }
    weights_.push_back(weight);

    if (graph_type == GraphType::kUndirectedGraph) {
      if (to != from) {
        adj_list_[to].push_back(id);
      }
      neighbours_[to].emplace_back(from, id);
    } else if (graph_type == GraphType::kDirectedGraph) {
      if (to != from) {
        adj_list_[to].push_back(id);
      }
      in_neighbours_[to].emplace_back(from, id);
    }

    return id;
  }

  /// Gets all valid edges.
  ///
  /// Edge is valid if is not deleted with EraseEdge method.
  ///
  /// @return Vector of valid edges
  std::vector<TEdge> ExistingEdges() const {
    std::vector<TEdge> edges_out;
    edges_out.reserve(edges_.size());
    for (const auto &edge : edges_) {
      if (edge.id == k_deleted_edge_id_) continue;
      edges_out.push_back(edge);
    }
    return edges_out;
  }

  /// Checks if edge is valid.
  ///
  /// Edge is valid if is created and if is not deleted.
  ///
  /// @return true if edge is valid, otherwise returns false
  bool IsEdgeValid(TSize edge_id) const {
    if (edge_id >= edges_.size()) {
      return false;
    }
    if (edges_[edge_id].id == k_deleted_edge_id_) {
      return false;
    }
    return true;
  }

  /// Removes edge from graph.
  ///
  /// Recommendation is to use this method only in the tests.
  ///
  /// @param[in] node_from node id of node on same edge
  /// @param[in] node_to node id of node on same edge
  void EraseEdge(TSize node_from, TSize node_to) {
    if (node_from >= nodes_.size()) {
      throw mg_exception::InvalidIDException();
    }
    if (node_to >= nodes_.size()) {
      throw mg_exception::InvalidIDException();
    }

    const auto [range_start, range_end] = nodes_to_edge_.equal_range(std::minmax(node_from, node_to));
    auto selected_it = nodes_to_edge_.end();
    for (auto it = range_start; it != range_end; ++it) {
      const TSize eid = it->second;
      if (!IsEdgeValid(eid)) continue;
      if (edges_[eid].from == node_from && edges_[eid].to == node_to) {
        selected_it = it;
        break;
      }
      if (edges_[eid].from == node_to && edges_[eid].to == node_from) {
        const bool is_undirected_entry =
            std::any_of(neighbours_[node_from].begin(), neighbours_[node_from].end(),
                        [eid](const TNeighbour &n) { return n.edge_id == eid; });
        if (is_undirected_entry && selected_it == nodes_to_edge_.end()) {
          selected_it = it;
        }
      }
    }
    if (selected_it == nodes_to_edge_.end()) {
      return;
    }

    const TSize edge_id = selected_it->second;
    edges_[edge_id].id = Graph::k_deleted_edge_id_;
    nodes_to_edge_.erase(selected_it);

    auto mem_edge_it = inner_to_memgraph_edge_id_.find(edge_id);
    if (mem_edge_it != inner_to_memgraph_edge_id_.end()) {
      memgraph_to_inner_edge_id_.erase(mem_edge_it->second);
      inner_to_memgraph_edge_id_.erase(mem_edge_it);
    }

    std::erase(adj_list_[node_from], edge_id);
    if (node_to != node_from) {
      std::erase(adj_list_[node_to], edge_id);
    }

    auto erase_by_edge_id = [edge_id](std::vector<TNeighbour> &vec) {
      std::erase_if(vec, [edge_id](const TNeighbour &n) { return n.edge_id == edge_id; });
    };
    erase_by_edge_id(neighbours_[node_from]);
    erase_by_edge_id(in_neighbours_[node_from]);
    if (node_to != node_from) {
      erase_by_edge_id(neighbours_[node_to]);
      erase_by_edge_id(in_neighbours_[node_to]);
    }
  }

  ///
  /// Returns the GraphView ID from Memgraph's internal ID
  ///
  /// @param node_id Memgraphs's inner ID
  ///
  TSize GetInnerNodeId(std::uint64_t memgraph_id) const override {
    if (memgraph_to_inner_id_.find(memgraph_id) == memgraph_to_inner_id_.end()) {
      throw mg_exception::InvalidIDException();
    }
    return memgraph_to_inner_id_.at(memgraph_id);
  }

  ///
  /// Returns the GraphView ID from Memgraph's internal ID
  ///
  /// @param node_id Memgraphs's inner ID
  ///
  std::optional<TSize> GetInnerNodeIdOpt(std::uint64_t memgraph_id) const override {
    auto it = memgraph_to_inner_id_.find(memgraph_id);
    if (it != memgraph_to_inner_id_.end()) {
      return it->second;
    }
    if (IsTransactional()) {
      throw mg_exception::InvalidIDException();
    }
    return std::nullopt;
  }

  ///
  /// Returns the Memgraph database ID from graph view
  ///
  /// @param node_id view's inner ID
  ///
  std::uint64_t GetMemgraphNodeId(TSize node_id) const override {
    if (inner_to_memgraph_id_.find(node_id) == inner_to_memgraph_id_.end()) {
      throw mg_exception::InvalidIDException();
    }
    return inner_to_memgraph_id_.at(node_id);
  }

  ///
  /// Returns all graph nodes’ Memgraph IDs
  ///
  std::unordered_set<std::uint64_t> GetMemgraphNodeIDs() const {
    std::unordered_set<std::uint64_t> memgraph_node_ids;
    for (const auto [memgraph_node_id, _] : this->memgraph_to_inner_id_) {
      memgraph_node_ids.insert(memgraph_node_id);
    }

    return memgraph_node_ids;
  }

  ///
  /// Returns the GraphView ID from Memgraph's internal edge ID
  ///
  /// @param memgraph_id Memgraphs's inner edge ID
  ///
  TSize GetInnerEdgeId(std::uint64_t memgraph_id) const override {
    if (memgraph_to_inner_edge_id_.find(memgraph_id) == memgraph_to_inner_edge_id_.end()) {
      throw mg_exception::InvalidIDException();
    }
    return memgraph_to_inner_edge_id_.at(memgraph_id);
  }

  ///
  /// Returns the Memgraph database edge ID from graph view
  ///
  /// @param edge_id view's inner edge ID
  ///
  std::uint64_t GetMemgraphEdgeId(TSize edge_id) const override {
    if (inner_to_memgraph_edge_id_.find(edge_id) == inner_to_memgraph_edge_id_.end()) {
      throw mg_exception::InvalidIDException();
    }
    return inner_to_memgraph_edge_id_.at(edge_id);
  }

  bool NodeExists(std::uint64_t memgraph_id) const override {
    return memgraph_to_inner_id_.find(memgraph_id) != memgraph_to_inner_id_.end();
  }

  bool Empty() const override { return nodes_.empty(); }

  /// Removes all edges and nodes from graph.
  void Clear() {
    adj_list_.clear();
    neighbours_.clear();
    in_neighbours_.clear();
    nodes_.clear();
    edges_.clear();
    weights_.clear();
    has_weights_ = false;
    memgraph_to_inner_id_.clear();
    inner_to_memgraph_id_.clear();
    memgraph_to_inner_edge_id_.clear();
    inner_to_memgraph_edge_id_.clear();
    nodes_to_edge_.clear();
  }

  void SetIsTransactional(bool is_transactional) { is_transactional_ = is_transactional; }

  bool IsTransactional() const { return is_transactional_; }

 private:
  // Constant is used for marking deleted edges.
  // If edge id is equal to constant, edge is deleted.
  static constexpr inline TSize k_deleted_edge_id_ = std::numeric_limits<TSize>::max();

  std::vector<std::vector<TSize>> adj_list_;
  std::vector<std::vector<TNeighbour>> neighbours_;  // only contains out-neighbours if the graph is directed
  std::vector<std::vector<TNeighbour>> in_neighbours_;

  std::vector<double> weights_;
  bool has_weights_ = false;

  std::vector<TNode> nodes_;
  std::vector<TEdge> edges_;

  std::unordered_map<TSize, std::uint64_t> inner_to_memgraph_id_;
  std::unordered_map<std::uint64_t, TSize> memgraph_to_inner_id_;

  std::unordered_map<TSize, std::uint64_t> inner_to_memgraph_edge_id_;
  std::unordered_map<std::uint64_t, TSize> memgraph_to_inner_edge_id_;

  std::multimap<std::pair<TSize, TSize>, TSize> nodes_to_edge_;

  bool is_transactional_ = false;
};
}  // namespace mg_graph
