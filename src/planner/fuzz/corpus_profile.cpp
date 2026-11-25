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

// Profiling tool for VM pattern matching.
//
// Usage:
//   ./corpus_profile <corpus_file> [--iterations N]

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "fuzz_pattern_corpus.hpp"

namespace memgraph::planner::core {

using fuzz::FuzzPatternSession;
using fuzz::FuzzSessionOptions;
using fuzz::FuzzSymbol;
using fuzz::FuzzTypedEGraph;
using fuzz::pattern_to_memgraph;
using fuzz::pattern_to_string;
using pattern::Match;
using pattern::MatcherContext;
using pattern::MatcherIndex;
using pattern::Pattern;
using rewrite::RewriteRule;
using rewrite::RuleContext;

inline auto parse_positive_iterations(std::string_view raw) -> std::optional<int> {
  if (raw.empty()) return std::nullopt;
  int value = 0;
  auto const [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), value);
  if (ec != std::errc{} || ptr != raw.data() + raw.size() || value <= 0) {
    return std::nullopt;
  }
  return value;
}

class ProfileRunner {
 public:
  bool setup(uint8_t const *data, std::size_t size) {
    FuzzSessionOptions opts;
    opts.run_oracle_checks = false;
    opts.skip_egglog = true;
    opts.abort_on_failure = false;

    session_ = std::make_unique<FuzzPatternSession>(std::move(opts));
    if (!session_->replay_stream(data, size)) {
      return false;
    }

    auto const &asts = session_->current_patterns_ast();
    if (asts.empty() || session_->created_ids().empty()) return false;

    session_->rebuild();

    patterns_.clear();
    for (auto const &ast : asts) {
      patterns_.push_back(pattern_to_memgraph(ast));
    }

    for (auto const &p : patterns_) {
      if (p.num_vars() == 0) return false;
    }

    std::vector<Pattern<FuzzSymbol>> patterns_copy;
    patterns_copy.reserve(asts.size());
    for (auto const &ast : asts) {
      patterns_copy.push_back(pattern_to_memgraph(ast));
    }

    auto rule_builder = RewriteRule<FuzzTypedEGraph>::Builder("profile_vm");
    for (auto &p : patterns_copy) {
      rule_builder = std::move(rule_builder).pattern(std::move(p));
    }
    vm_rule_ = std::make_unique<RewriteRule<FuzzTypedEGraph>>(
        std::move(rule_builder).apply([this](RuleContext<FuzzTypedEGraph> &, Match const &) { ++match_count_; }));

    auto const &egraph = session_->egraph();
    std::cerr << "E-graph: " << egraph.num_classes() << " classes, " << egraph.num_nodes() << " nodes\n";
    std::cerr << "Patterns:\n";
    for (std::size_t i = 0; i < asts.size(); ++i) {
      std::cerr << "  " << (i + 1) << ": " << pattern_to_string(asts[i]) << "\n";
    }

    return true;
  }

  void run_vm(int iterations) {
    if (iterations <= 0) {
      throw std::invalid_argument("iterations must be positive");
    }
    auto &egraph = session_->egraph();
    auto &typed_egraph = session_->typed_egraph();

    MatcherIndex index(egraph);
    pattern::vm::VMExecutor vm_executor(egraph);
    MatcherContext matcher_ctx;  // NOLINT(misc-const-correctness)
    std::vector<EClassId> new_eclasses;
    RuleContext<FuzzTypedEGraph> rule_ctx(typed_egraph, new_eclasses);

    for (int i = 0; i < iterations; ++i) {
      match_count_ = 0;
      vm_rule_->match(index, vm_executor, matcher_ctx);
      rule_ctx.reset_rewrites();
      vm_rule_->apply(rule_ctx, matcher_ctx);
    }
  }

  [[nodiscard]] auto match_count() const -> std::size_t { return match_count_; }
  [[nodiscard]] auto session() const -> FuzzPatternSession const & { return *session_; }

 private:
  std::unique_ptr<FuzzPatternSession> session_;
  std::vector<Pattern<FuzzSymbol>> patterns_;
  std::unique_ptr<RewriteRule<FuzzTypedEGraph>> vm_rule_;
  std::size_t match_count_ = 0;
};

}  // namespace memgraph::planner::core

int main(int argc, char *argv[]) try {
  using namespace memgraph::planner::core;

  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <corpus_file> [--iterations N]\n";
    return 1;
  }

  std::string const filename = argv[1];
  int iterations = 10000;

  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--iterations") == 0) {
      if (i + 1 >= argc) {
        std::cerr << "Missing value after --iterations\n";
        return 1;
      }
      auto parsed = parse_positive_iterations(argv[++i]);
      if (!parsed) {
        std::cerr << "Invalid --iterations value '" << argv[i] << "': expected a positive integer\n";
        return 1;
      }
      iterations = *parsed;
    } else {
      std::cerr << "Unknown argument: " << argv[i] << "\n";
      return 1;
    }
  }

  std::ifstream file(filename, std::ios::binary);
  if (!file) {
    std::cerr << "Failed to open " << filename << "\n";
    return 1;
  }

  std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

  ProfileRunner runner;
  if (!runner.setup(data.data(), data.size())) {
    std::cerr << "Failed to setup profiling (no valid pattern or e-graph)\n";
    return 1;
  }

  std::cerr << "Running " << iterations << " iterations\n";

  auto start = std::chrono::high_resolution_clock::now();

  std::cerr << "Running VM...\n";
  runner.run_vm(iterations);

  auto end = std::chrono::high_resolution_clock::now();
  auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

  std::cerr << "Done in " << duration_ms << "ms\n";
  std::cerr << "Matches per iteration: " << runner.match_count() << "\n";

  return 0;
} catch (std::exception const &e) {
  std::cerr << "Error: " << e.what() << "\n";
  return 1;
}
