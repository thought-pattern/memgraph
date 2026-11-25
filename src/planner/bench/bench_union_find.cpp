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

#include <array>
#include <vector>

#include <benchmark/benchmark.h>

import memgraph.planner.core.union_find;

constexpr auto kRangeLow = 64;
constexpr auto kRangeHigh = 1024 * 4;

namespace {

using memgraph::planner::core::UnionFind;
using memgraph::planner::core::UnionFindContext;

void BM_UnionFind_MakeSet(benchmark::State &state) {
  UnionFind uf;
  for (auto _ : state) {  // NOLINT(clang-analyzer-deadcode.DeadStores)
    uf.Clear();
    for (int i = 0; i < state.range(0); ++i) {
      benchmark::DoNotOptimize(uf.MakeSet());
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * state.range(0));
  state.SetComplexityN(state.range(0));
}

void BM_UnionFind_Find(benchmark::State &state) {
  UnionFind uf;
  std::vector<uint32_t> ids;

  ids.reserve(state.range(0));
  for (int i = 0; i < state.range(0); ++i) {
    ids.push_back(uf.MakeSet());
  }

  // Create some unions to make find work harder
  for (int i = 0; i < state.range(0) / 2; ++i) {
    uf.UnionSets(ids[i], ids[i + state.range(0) / 2]);
  }

  for (auto _ : state) {  // NOLINT(clang-analyzer-deadcode.DeadStores)
    for (auto id : ids) {
      benchmark::DoNotOptimize(uf.Find(id));
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * ids.size()));
  state.SetComplexityN(state.range(0));
}

void BM_UnionFind_Find_OnAChain(benchmark::State &state) {
  constexpr std::size_t kTreesPerSample = 16;
  auto const n = static_cast<std::size_t>(state.range(0));
  std::size_t expected_depth = 0;
  for (std::size_t span = 1; (span * 2) <= n; span *= 2) {
    ++expected_depth;
  }

  std::array<UnionFind, kTreesPerSample> trees;
  std::vector<uint32_t> ids;
  ids.reserve(n);

  for (auto _ : state) {  // NOLINT(clang-analyzer-deadcode.DeadStores)
    state.PauseTiming();
    for (auto &uf : trees) {
      uf.Clear();
      ids.clear();
      for (std::size_t i = 0; i < n; ++i) {
        ids.push_back(uf.MakeSet());
      }
      // Pairwise tournament unions build a binomial tree of height floor(log2(n))
      // with ids[0] at the deepest leaf, without compressing ids[0] during setup.
      for (std::size_t stride = 1; stride < n; stride *= 2) {
        for (std::size_t idx = stride - 1; idx + stride < n; idx += stride * 2) {
          uf.UnionSets(ids[idx], ids[idx + stride]);
        }
      }
      if (uf.Depth(ids[0]) != expected_depth) {
        state.SkipWithError("UnionFind tree depth did not match expected logarithmic chain height");
        return;
      }
    }
    state.ResumeTiming();

    for (auto &uf : trees) {
      benchmark::DoNotOptimize(uf.Find(0));
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * kTreesPerSample));
  state.SetComplexityN(state.range(0));
}

void BM_UnionFind_Union(benchmark::State &state) {
  UnionFind uf;
  std::vector<uint32_t> ids;
  for (auto _ : state) {  // NOLINT(clang-analyzer-deadcode.DeadStores)
    state.PauseTiming();
    uf.Clear();
    ids.clear();

    ids.reserve(state.range(0));
    for (int i = 0; i < state.range(0); ++i) {
      ids.push_back(uf.MakeSet());
    }
    state.ResumeTiming();

    for (size_t i = 0; i < (state.range(0) - 1); ++i) {
      benchmark::DoNotOptimize(uf.UnionSets(ids[i], ids[i + 1]));
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * ids.size()));
  state.SetComplexityN(state.range(0));
}

void BM_UnionFind_BulkUnion(benchmark::State &state) {
  UnionFind uf;
  std::vector<uint32_t> ids;
  UnionFindContext ctx;

  auto size = state.range(0);
  for (auto _ : state) {  // NOLINT(clang-analyzer-deadcode.DeadStores)
    state.PauseTiming();
    uf.Clear();
    ids.clear();

    ids.reserve(size);
    for (int64_t i = 0; i < size; ++i) {
      ids.push_back(uf.MakeSet());
    }

    state.ResumeTiming();

    uf.UnionSets(ids, ctx);

    benchmark::DoNotOptimize(uf);
  }

  state.SetComplexityN(size);
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * size);
}

}  // namespace

BENCHMARK(BM_UnionFind_MakeSet)->Name("UnionFind/MakeSet")->Range(kRangeLow, kRangeHigh)->ArgName("n")->Complexity();
BENCHMARK(BM_UnionFind_Find)->Name("UnionFind/Find")->Range(kRangeLow, kRangeHigh)->ArgName("n")->Complexity();
BENCHMARK(BM_UnionFind_Find_OnAChain)
    ->Name("UnionFind/Find_OnAChain")
    ->Range(kRangeLow, kRangeHigh)
    ->ArgName("n")
    ->Complexity();
BENCHMARK(BM_UnionFind_Union)->Name("UnionFind/Union")->Range(kRangeLow, kRangeHigh)->ArgName("n")->Complexity();
BENCHMARK(BM_UnionFind_BulkUnion)
    ->Name("UnionFind/BulkUnion")
    ->Range(kRangeLow, kRangeHigh)
    ->ArgName("n")
    ->Complexity();
