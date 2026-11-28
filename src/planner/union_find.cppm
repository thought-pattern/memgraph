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

module;

#include <bit>
#include <cassert>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef ASSERT_FUZZ
#undef assert
#define assert(expr)                                                                                   \
  ((expr) ? (void)0                                                                                    \
          : throw std::runtime_error(std::string("Assertion failed: ") + #expr + " at " __FILE__ ":" + \
                                     std::to_string(__LINE__)))
#endif

export module memgraph.planner.core.union_find;

export namespace memgraph::planner::core {

template <std::unsigned_integral IdT = uint32_t>
struct BasicUnionFind;

using UnionFind = BasicUnionFind<uint32_t>;

/**
 * @brief Processing context for UnionFind operations
 *
 * @details
 * Provides reusable temporary storage specifically for UnionFind operations,
 * eliminating allocation overhead in hot paths.
 */
struct UnionFindContext {
  UnionFindContext() = default;

 private:
  template <std::unsigned_integral>
  friend struct BasicUnionFind;

  auto GetReusableVector(std::size_t n) -> std::vector<uint32_t> & {
    roots_.clear();
    roots_.reserve(n);
    return roots_;
  }

  std::vector<uint32_t> roots_;
};

/**
 * @brief Union-Find data structure
 *
 * @details
 * Implements a disjoint-set data structure.
 *
 * @par Implementation Notes:
 * - **Path halving**: Amortized O(α(n)) find operations with single-pass optimization
 * - **Union by rank**: Keeps trees balanced for optimal performance
 * - **Bulk operations**: Zero-allocation UnionSets()
 * - **8-bit rank storage**: Memory-optimized rank storage (1 byte vs 8 bytes)
 * - **Chunked deque storage**: Eliminates vector reallocation overhead
 *
 * @par Thread Safety:
 * Not thread-safe. External synchronization required for concurrent access.
 *
 * @complexity
 * - Space: O(n) where n is the number of sets created
 * - Time: O(α(n)) amortized for find/union operations
 */
template <std::unsigned_integral IdT>
struct BasicUnionFind {
  using id_t = IdT;
  using rank_t = uint8_t;

  static_assert(std::bit_width(std::numeric_limits<id_t>::max()) < std::numeric_limits<rank_t>::max(),
                "8-bit rank storage insufficient for expected maximum tree depth");

  BasicUnionFind() = default;

  /**
   * @brief Create a new set containing only the returned ID
   *
   * @return ID of the new set
   */
  auto MakeSet() -> id_t {
    if (parent_.size() > static_cast<std::size_t>(std::numeric_limits<id_t>::max())) {
      throw std::overflow_error("UnionFind ID space exhausted");
    }
    auto const id = static_cast<id_t>(parent_.size());
    parent_.push_back(id);
    try {
      rank_.push_back(0);
    } catch (...) {
      parent_.pop_back();
      throw;
    }
    ++num_of_sets_;
    return id;
  }

  /**
   * @brief Roll back the most recently allocated unmerged singleton set
   */
  void PopLastSingleton() {
    assert(!parent_.empty());
    assert(parent_.back() == static_cast<id_t>(parent_.size() - 1));
    assert(rank_.back() == 0);
    parent_.pop_back();
    rank_.pop_back();
    --num_of_sets_;
  }

  /**
   * @brief Find root id of the set
   *
   * @param id The node ID to find the root for
   * @return root id of the set
   *
   * @complexity O(α(n)) amortized where α is inverse Ackermann function
   */
  auto Find(id_t id) -> id_t {
    assert(id < parent_.size());
    auto current = id;
    while (parent_[current] != current) [[likely]] {
      auto const next = parent_[current];
      parent_[current] = parent_[next];
      current = next;
    }
    return current;
  }

  /**
   * @brief Find root id of the set without path halving
   *
   * @param id The node ID to find the root for
   * @return root id of the set
   */
  [[deprecated("Use the non-const version that performs path halving")]] [[nodiscard]] auto Find(id_t id) const -> id_t {
    assert(id < parent_.size());
    auto current = id;
    while (parent_[current] != current) {
      current = parent_[parent_[current]];
    }
    return current;
  }

  /**
   * @brief Merges two sets
   */
  auto UnionSets(id_t a, id_t b) -> id_t {
    auto root_a = Find(a);
    auto root_b = Find(b);

    if (root_a == root_b) [[unlikely]] {
      return root_a;
    }

    --num_of_sets_;

    auto const rank_comparison = rank_[root_a] <=> rank_[root_b];
    auto const a_is_parent = rank_comparison == std::strong_ordering::greater;
    auto const parent_root = a_is_parent ? root_a : root_b;
    auto const child_root = a_is_parent ? root_b : root_a;

    parent_[child_root] = parent_root;
    if (rank_comparison == std::strong_ordering::equivalent) {
      ++rank_[parent_root];
    }
    return parent_root;
  }

  /**
   * @brief Efficiently merges multiple sets
   */
  auto UnionSets(std::span<const id_t> ids, UnionFindContext &ctx) -> id_t {
    assert(!ids.empty());

    if (ids.size() == 1) [[unlikely]] {
      return Find(ids[0]);
    }

    auto &roots = ctx.GetReusableVector(ids.size());
    for (auto id : ids) {
      roots.push_back(static_cast<uint32_t>(Find(id)));
    }

    auto best_root = static_cast<id_t>(roots[0]);
    auto best_rank = rank_[best_root];
    bool unique_best = true;

    for (size_t i = 1; i < roots.size(); ++i) {
      auto root = static_cast<id_t>(roots[i]);
      auto current_rank = rank_[root];

      auto rank_diff = static_cast<int64_t>(current_rank) - static_cast<int64_t>(best_rank);
      auto is_higher_rank = rank_diff > 0;
      auto is_equal_rank = rank_diff == 0;
      auto is_different_root = root != best_root;

      best_root = is_higher_rank ? root : best_root;
      best_rank = is_higher_rank ? current_rank : best_rank;
      unique_best = is_higher_rank || (unique_best && !(is_equal_rank && is_different_root));
    }

    size_t merges_performed = 0;
    for (auto raw_root : roots) {
      auto const root = static_cast<id_t>(raw_root);
      if (parent_[root] != best_root) {
        parent_[root] = best_root;
        ++merges_performed;
      }
    }
    num_of_sets_ -= merges_performed;

    if (!unique_best) {
      ++rank_[best_root];
    }

    return best_root;
  }

  /**
   * @brief Test if two elements are in the same set
   */
  auto Connected(id_t a, id_t b) -> bool {
    assert(a < parent_.size());
    assert(b < parent_.size());
    return Find(a) == Find(b);
  }

  /**
   * @brief Get total number of elements
   */
  [[nodiscard]] auto Size() const -> std::size_t { return parent_.size(); }

  /**
   * @brief Get number of distinct sets
   */
  [[nodiscard]] auto ComponentCount() const -> size_t { return num_of_sets_; }

  /**
   * @brief Return uncompressed parent-pointer hop count from `id` to its root
   */
  [[nodiscard]] auto Depth(id_t id) const -> std::size_t {
    assert(id < parent_.size());
    std::size_t depth = 0;
    auto current = id;
    while (parent_[current] != current) {
      current = parent_[current];
      ++depth;
    }
    return depth;
  }

  /**
   * @brief Reset to an empty union find structure
   */
  void Clear() {
    parent_.clear();
    rank_.clear();
    num_of_sets_ = 0;
  }

 private:
  std::deque<id_t> parent_;
  std::deque<rank_t> rank_;
  size_t num_of_sets_ = 0;
};
}  // namespace memgraph::planner::core
