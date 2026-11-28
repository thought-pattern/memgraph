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

#pragma once

#include <bit>
#include <compare>
#include <cstdint>
#include <utility>

#include "storage/v2/id_types.hpp"

namespace memgraph::storage {

struct Edge;

struct EdgeRef {
  explicit EdgeRef(Gid gid) noexcept : gid(gid) {}

  explicit EdgeRef(Edge *ptr) noexcept : ptr(ptr) {}

  [[nodiscard]] std::uintptr_t RawBits() const noexcept { return std::bit_cast<std::uintptr_t>(*this); }

  friend bool operator==(const EdgeRef &lhs, const EdgeRef &rhs) noexcept { return lhs.RawBits() == rhs.RawBits(); }

  friend std::strong_ordering operator<=>(const EdgeRef &lhs, const EdgeRef &rhs) noexcept {
    return lhs.RawBits() <=> rhs.RawBits();
  }

  union {
    Gid gid;
    Edge *ptr;
  };

  template <typename H>
  friend H AbslHashValue(H h, EdgeRef const &edge_ref) {
    return H::combine(std::move(h), edge_ref.RawBits());
  }
};

}  // namespace memgraph::storage
