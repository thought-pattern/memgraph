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

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>

#include "utils/db_aware_allocator.hpp"

namespace memgraph::utils {

// custom allocator, ensures all allocations are page aligned.
// Allocations follow the DB arena currently pinned in TLS, so callers must
// establish DbArenaScope / DbAwareThread before using containers that rely on
// this allocator.
template <typename T>
struct PageAlignedAllocator {
  static constexpr std::size_t PAGE_SIZE = 4096;
  static constexpr std::size_t MAX_ALIGNED_SIZE = (std::numeric_limits<std::size_t>::max() / PAGE_SIZE) * PAGE_SIZE;
  using value_type = T;

  PageAlignedAllocator() = default;

  template <class U>
  explicit PageAlignedAllocator(const PageAlignedAllocator<U> & /*other*/) noexcept {}

  static constexpr std::size_t ComputeAlignedSize(std::size_t n) {
    if (sizeof(T) > 0 && n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw std::bad_array_new_length();
    }
    const auto raw_size = std::max(n * sizeof(T), PAGE_SIZE);
    if (raw_size > MAX_ALIGNED_SIZE) {
      throw std::bad_alloc();
    }
    return ((raw_size - 1) / PAGE_SIZE + 1) * PAGE_SIZE;
  }

  auto allocate(std::size_t n) -> T * {
    const auto size = ComputeAlignedSize(n);
    return static_cast<T *>(memory::DbAllocateBytes(size, memory::tls_db_arena_state.arena, PAGE_SIZE));
  }

  void deallocate(T *p, std::size_t n) const noexcept {
    // NOTE: jemalloc tracks the owning arena per-extent in its own metadata, so GC can safely
    // free query-thread allocations regardless of which thread calls deallocate.
    // Recalculate the actual allocated size (mirroring allocate) for sized deallocation.
    const auto raw_size = std::max(n * sizeof(T), PAGE_SIZE);
    const auto size = ((raw_size - 1) / PAGE_SIZE + 1) * PAGE_SIZE;
    memory::DbDeallocateBytes(static_cast<void *>(p), size, PAGE_SIZE);
  }

  friend bool operator==(PageAlignedAllocator const &, PageAlignedAllocator const &) noexcept { return true; }

  friend bool operator!=(PageAlignedAllocator const &lhs, PageAlignedAllocator const &rhs) noexcept {
    return !(lhs == rhs);
  }
};

}  // namespace memgraph::utils
