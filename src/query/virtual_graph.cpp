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

#include "query/virtual_graph.hpp"

#include "utils/logging.hpp"

namespace memgraph::query {

void VirtualGraph::CopyFrom(const VirtualGraph &other) {
  auto *resource = get_allocator().resource();
  std::pmr::polymorphic_allocator<VirtualNode> node_poly_alloc(resource);
  const VirtualEdge::allocator_type edge_alloc(resource);

  node_map new_nodes(get_allocator());
  new_nodes.reserve(other.nodes_.size());
  for (const auto &[gid, sp] : other.nodes_) {
    auto cloned = std::allocate_shared<VirtualNode>(node_poly_alloc, *sp);
    cloned->impl_->in_degree = 0;
    cloned->impl_->out_degree = 0;
    new_nodes.try_emplace(gid, std::move(cloned));
  }

  // Fallback map in case an edge references a VirtualNode not present in other.nodes_
  node_map extra_nodes(get_allocator());
  auto resolve_node = [&](const VirtualNode &endpoint) -> std::shared_ptr<const VirtualNode> {
    if (const auto it = new_nodes.find(endpoint.Gid()); it != new_nodes.end()) {
      return it->second;
    }
    if (const auto it = extra_nodes.find(endpoint.Gid()); it != extra_nodes.end()) {
      return it->second;
    }
    auto cloned = std::allocate_shared<VirtualNode>(node_poly_alloc, endpoint);
    cloned->impl_->in_degree = 0;
    cloned->impl_->out_degree = 0;
    return extra_nodes.try_emplace(endpoint.Gid(), std::move(cloned)).first->second;
  };

  edge_set new_edges(get_allocator());
  new_edges.reserve(other.edges_.size());
  for (const auto &edge : other.edges_) {
    auto from_sp = resolve_node(edge.From());
    auto to_sp = resolve_node(edge.To());
    const auto [it, inserted] = new_edges.insert(VirtualEdge(edge, std::move(from_sp), std::move(to_sp), edge_alloc));
    if (inserted) {
      ++it->From().impl_->out_degree;
      ++it->To().impl_->in_degree;
    }
  }

  nodes_ = std::move(new_nodes);
  edges_ = std::move(new_edges);
  out_index_.clear();
  in_index_.clear();
  RebuildEdgeIndexes();
}

VirtualGraph::VirtualGraph(const VirtualGraph &other, allocator_type alloc)
    : nodes_(alloc), edges_(alloc), out_index_(alloc), in_index_(alloc) {
  CopyFrom(other);
}

VirtualGraph &VirtualGraph::operator=(const VirtualGraph &other) {
  if (this == &other) return *this;
  CopyFrom(other);
  return *this;
}

VirtualGraph &VirtualGraph::operator=(VirtualGraph &&other) {
  if (this == &other) return *this;
  if (get_allocator() == other.get_allocator()) {
    nodes_ = std::move(other.nodes_);
    edges_ = std::move(other.edges_);
    out_index_ = std::move(other.out_index_);
    in_index_ = std::move(other.in_index_);
  } else {
    CopyFrom(other);
    other.nodes_.clear();
    other.edges_.clear();
    other.out_index_.clear();
    other.in_index_.clear();
  }
  return *this;
}

VirtualGraph::VirtualGraph(VirtualGraph &&other, allocator_type alloc)
    : nodes_(alloc), edges_(alloc), out_index_(alloc), in_index_(alloc) {
  if (alloc == other.nodes_.get_allocator()) {
    nodes_ = std::move(other.nodes_);
    edges_ = std::move(other.edges_);
    out_index_ = std::move(other.out_index_);
    in_index_ = std::move(other.in_index_);
  } else {
    CopyFrom(other);
    other.nodes_.clear();
    other.edges_.clear();
    other.out_index_.clear();
    other.in_index_.clear();
  }
}

void VirtualGraph::IndexEdge(const VirtualEdge *edge) {
  out_index_[edge->FromGid()].push_back(edge);
  in_index_[edge->ToGid()].push_back(edge);
}

void VirtualGraph::RebuildEdgeIndexes() {
  for (const auto &edge : edges_) IndexEdge(&edge);
}

const VirtualNode &VirtualGraph::InsertNode(VirtualNode node) {
  const auto synthetic_gid = node.Gid();
  auto *resource = nodes_.get_allocator().resource();
  node.impl_->in_degree = 0;
  node.impl_->out_degree = 0;
  auto shared =
      std::allocate_shared<VirtualNode>(std::pmr::polymorphic_allocator<VirtualNode>(resource), std::move(node));
  const auto [it, inserted] = nodes_.try_emplace(synthetic_gid, std::move(shared));
  DMG_ASSERT(inserted, "NextSyntheticGid counter collision: synthetic gid not unique");
  return *it->second;
}

std::shared_ptr<const VirtualNode> VirtualGraph::FindNode(storage::Gid synthetic_gid) const {
  if (const auto it = nodes_.find(synthetic_gid); it != nodes_.end()) return it->second;
  return {};
}

bool VirtualGraph::InsertEdgeIfNew(VirtualEdge edge) {
  const auto [it, inserted] = edges_.insert(std::move(edge));
  if (!inserted) return false;
  ++it->From().impl_->out_degree;
  ++it->To().impl_->in_degree;
  IndexEdge(&*it);
  return true;
}

std::span<const VirtualEdge *const> VirtualGraph::OutEdges(storage::Gid vertex_gid) const {
  if (const auto it = out_index_.find(vertex_gid); it != out_index_.end()) return it->second;
  return {};
}

std::span<const VirtualEdge *const> VirtualGraph::InEdges(storage::Gid vertex_gid) const {
  if (const auto it = in_index_.find(vertex_gid); it != in_index_.end()) return it->second;
  return {};
}

void VirtualGraph::Merge(const VirtualGraph &other, const VirtualGraphAliasMap &aliases) {
  auto *resource = get_allocator().resource();
  std::pmr::polymorphic_allocator<VirtualNode> node_poly_alloc(resource);
  for (const auto &[synth_gid, node] : other.nodes_) {
    if (aliases.contains(synth_gid) || nodes_.contains(synth_gid)) continue;
    auto cloned = std::allocate_shared<VirtualNode>(node_poly_alloc, *node);
    cloned->impl_->in_degree = 0;
    cloned->impl_->out_degree = 0;
    nodes_.try_emplace(synth_gid, std::move(cloned));
  }
  const auto resolve = [&aliases](storage::Gid g) {
    if (const auto it = aliases.find(g); it != aliases.end()) return it->second;
    return g;
  };
  const VirtualEdge::allocator_type edge_alloc(get_allocator().resource());
  for (const auto &edge : other.edges_) {
    auto from_shared = FindNode(resolve(edge.FromGid()));
    auto to_shared = FindNode(resolve(edge.ToGid()));
    DMG_ASSERT(from_shared && to_shared, "merge alias resolution failed");
    InsertEdgeIfNew(VirtualEdge(edge, std::move(from_shared), std::move(to_shared), edge_alloc));
  }
}

}  // namespace memgraph::query
