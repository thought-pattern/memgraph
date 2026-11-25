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

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "fuzz_common.hpp"
#include "planner/pattern/match.hpp"
#include "planner/pattern/match_index.hpp"
#include "planner/pattern/vm/compiler.hpp"
#include "planner/pattern/vm/executor.hpp"
#include "planner/pattern/vm/tracer.hpp"
#include "planner/rewrite/rule.hpp"

import memgraph.planner.core.egraph;

namespace memgraph::planner::core::fuzz {

inline constexpr std::size_t kMaxPatternVariables = 20;

inline auto quote_shell_arg(std::string_view raw) -> std::string {
  std::string out{"'"};
  for (char ch : raw) {
    if (ch == '\'') {
      out += "'\\''";
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('\'');
  return out;
}

class EgglogGenerator {
 public:
  auto emit_leaf(FuzzSymbol sym, uint64_t disambiguator) -> std::string {
    auto var_name = fmt::format("n{}", var_counter_++);
    program_ << fmt::format("(let {} ({} {}))\n", var_name, symbol_name(sym), disambiguator);
    return var_name;
  }

  auto emit_unary(FuzzSymbol sym, std::string_view child) -> std::string {
    auto var_name = fmt::format("n{}", var_counter_++);
    program_ << fmt::format("(let {} ({} {}))\n", var_name, symbol_name(sym), child);
    return var_name;
  }

  auto emit_binary(FuzzSymbol sym, std::string_view left, std::string_view right) -> std::string {
    auto var_name = fmt::format("n{}", var_counter_++);
    program_ << fmt::format("(let {} ({} {} {}))\n", var_name, symbol_name(sym), left, right);
    return var_name;
  }

  auto emit_ternary(FuzzSymbol sym, std::string_view c0, std::string_view c1, std::string_view c2) -> std::string {
    auto var_name = fmt::format("n{}", var_counter_++);
    program_ << fmt::format("(let {} ({} {} {} {}))\n", var_name, symbol_name(sym), c0, c1, c2);
    return var_name;
  }

  void emit_merge(std::string_view id1, std::string_view id2) {
    program_ << fmt::format("(union {} {})\n", id1, id2);
  }

  [[nodiscard]] auto generate() const -> std::string { return program_.str(); }

  void clear() {
    program_.str("");
    program_.clear();
    var_counter_ = 0;
  }

 private:
  std::ostringstream program_;
  int var_counter_ = 0;
};

struct EgglogResult {
  bool success = false;
  bool truncated = false;
  std::size_t match_count = 0;
  int exit_code = -1;
  std::string temp_path_used;
  std::string output;
  std::string error;
};

inline auto run_egglog(std::string_view program, std::size_t print_limit, std::string_view egglog_bin = "egglog")
    -> EgglogResult {
  EgglogResult result;

  auto tmpl_path = std::filesystem::temp_directory_path() / "fuzz_pattern_vm_XXXXXX.egg";
  std::string tmpl = tmpl_path.string();
  int const fd = ::mkstemps(tmpl.data(), 4);
  if (fd < 0) {
    result.error = "Failed to create private temporary reference file";
    return result;
  }
  result.temp_path_used = tmpl;

  struct TempFileGuard {
    std::string path;
    ~TempFileGuard() {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  } guard{tmpl};

  std::size_t total_written = 0;
  while (total_written < program.size()) {
    auto const n = ::write(fd, program.data() + total_written, program.size() - total_written);
    if (n <= 0) {
      (void)::close(fd);
      result.error = "Failed to write complete program to temporary reference file";
      return result;
    }
    total_written += static_cast<std::size_t>(n);
  }
  if (::close(fd) != 0) {
    result.error = "Failed to flush and close temporary reference file";
    return result;
  }

  std::string const cmd = fmt::format("{} {} 2>&1", quote_shell_arg(egglog_bin), quote_shell_arg(tmpl));
  FILE *pipe = ::popen(cmd.c_str(), "r");
  if (pipe == nullptr) {
    result.error = "Failed to invoke egglog reference process";
    return result;
  }

  std::array<char, 4096> buffer{};
  std::ostringstream output;
  while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    output << buffer.data();
  }

  int const status = ::pclose(pipe);
  result.output = output.str();

  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  }

  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    result.success = true;

    std::istringstream iss(result.output);
    std::string line;
    bool saw_omission_marker = false;
    while (std::getline(iss, line)) {
      auto pos = line.find_first_not_of(" \t\r");
      if (pos == std::string::npos) continue;
      auto trimmed = line.substr(pos);
      if (trimmed.starts_with("(MatchResult")) {
        ++result.match_count;
      } else if (trimmed == "..." || trimmed.starts_with("... ") || trimmed.find("omitted") != std::string::npos) {
        saw_omission_marker = true;
      }
    }
    result.truncated = saw_omission_marker || (print_limit > 0 && result.match_count >= print_limit);
  } else {
    result.error = fmt::format("Reference tool exited unsuccessfully (status={}, exit_code={}): {}", status,
                               result.exit_code, result.output);
  }

  return result;
}

struct FuzzSessionOptions {
  bool verbose = false;
  bool skip_egglog = false;
  bool run_oracle_checks = true;
  bool abort_on_failure = true;
  std::string egglog_bin = "egglog";
};

enum class VerifyOutcome : uint8_t {
  Verified,
  SkippedNoPatternOrEmpty,
  SkippedUnsupportedPattern,
  SkippedOracleDisabled,
  OracleToolFailure,
  Mismatch,
};

class FuzzPatternSession {
 public:
  explicit FuzzPatternSession(FuzzSessionOptions opts = {}) : opts_(std::move(opts)) {}

  bool replay_stream(uint8_t const *data, std::size_t size) {
    std::size_t pos = 0;
    if (!execute_operation(0, data, pos, size)) {
      return false;
    }
    while (pos < size && pos < 1000) {
      uint8_t const op = data[pos++];
      if (!execute_operation(op, data, pos, size)) {
        return false;
      }
    }
    return true;
  }

  bool execute_operation(uint8_t op, uint8_t const *data, std::size_t &pos, std::size_t size) {
    ++operation_count_;
    if (opts_.verbose) {
      std::cerr << "\n--- Operation #" << operation_count_ << " ---\n";
    }

    static constexpr std::array<uint8_t, 9> kDispatch = {
        0,  // CREATE_LEAF        (1/9)
        1,
        1,
        1,  // CREATE_COMPOUND    (3/9)
        2,  // MERGE              (1/9)
        3,  // REBUILD            (1/9)
        4,  // SET_PATTERN        (1/9)
        6,  // VERIFY_ABSENT      (1/9)
        7,  // APPLY_REWRITE      (1/9)
    };
    int const operation_type = kDispatch[op % kDispatch.size()];

    switch (operation_type) {
      case 0:
        return create_leaf_node(data, pos, size);
      case 1:
        return create_compound_node(data, pos, size);
      case 2:
        return merge_classes(data, pos, size);
      case 3:
        return rebuild();
      case 4:
        return set_pattern(data, pos, size);
      case 5:
        return run_configured_match_check();
      case 6:
        return verify_absent();
      case 7:
        return apply_rewrite_cycle(data, pos, size);
      default:
        return true;
    }
  }

  bool create_leaf_node(uint8_t const *data, std::size_t &pos, std::size_t size) {
    if (pos >= size) return true;

    uint8_t const symbol = data[pos++] % 5;  // A-E

    uint64_t disambiguator = 0;
    for (int byte_idx = 0; byte_idx < 4 && pos < size; ++byte_idx) {
      disambiguator = (disambiguator << 8) | data[pos++];
    }

    auto sym = static_cast<FuzzSymbol>(symbol);
    auto info = egraph_.emplace(sym, disambiguator);
    created_ids_.push_back(info.eclass_id);

    auto var_name = egglog_.emit_leaf(sym, disambiguator);
    egglog_names_[info.eclass_id] = var_name;
    return true;
  }

  bool create_compound_node(uint8_t const *data, std::size_t &pos, std::size_t size) {
    if (pos >= size || created_ids_.empty()) return true;

    static constexpr std::array<uint8_t, 10> kSymDispatch = {
        0,  // F       arity 1
        1,  // G       arity 1
        2,  // H       arity 1
        3,
        3,  // Plus    arity 2
        4,
        4,  // Mul     arity 2
        5,
        5,
        5,  // Ternary arity 3
    };
    auto sym = static_cast<FuzzSymbol>(10 + kSymDispatch[data[pos++] % kSymDispatch.size()]);

    std::size_t arity = 1;
    if (sym == FuzzSymbol::Ternary) {
      arity = 3;
    } else if (sym == FuzzSymbol::Plus || sym == FuzzSymbol::Mul) {
      arity = 2;
    }

    if (created_ids_.size() < arity) return true;

    utils::small_vector<EClassId> children;
    std::vector<std::string> child_names;
    for (std::size_t i = 0; i < arity; ++i) {
      auto idx = pick_node_idx(data, pos, size);
      if (!idx) return true;
      auto child_id = egraph_.find(created_ids_[*idx]);
      children.push_back(child_id);
      child_names.push_back(egglog_names_[created_ids_[*idx]]);
    }

    auto info = egraph_.emplace(sym, children);
    created_ids_.push_back(info.eclass_id);
    // Record symbol presence only after insertion actually succeeded.
    created_symbols_.insert(sym);

    std::string var_name;
    if (arity == 1) {
      var_name = egglog_.emit_unary(sym, child_names[0]);
    } else if (arity == 2) {
      var_name = egglog_.emit_binary(sym, child_names[0], child_names[1]);
    } else {
      var_name = egglog_.emit_ternary(sym, child_names[0], child_names[1], child_names[2]);
    }
    egglog_names_[info.eclass_id] = var_name;
    return true;
  }

  bool merge_classes(uint8_t const *data, std::size_t &pos, std::size_t size) {
    if (created_ids_.size() < 2) return true;

    auto idx1 = pick_node_idx(data, pos, size);
    if (!idx1) return true;
    auto idx2 = pick_node_idx(data, pos, size);
    if (!idx2) return true;
    if (*idx1 == *idx2) return true;

    auto id1 = created_ids_[*idx1];
    auto id2 = created_ids_[*idx2];

    egraph_.merge(id1, id2);
    egglog_.emit_merge(egglog_names_[id1], egglog_names_[id2]);
    return true;
  }

  bool rebuild() {
    egraph_.rebuild(ctx_);
    return true;
  }

  bool set_pattern(uint8_t const *data, std::size_t &pos, std::size_t size) {
    if (pos >= size) return true;

    auto result = pattern_gen_.generate(data, pos, size);
    current_patterns_ast_ = std::move(result.patterns);
    current_shared_vars_ = std::move(result.shared_vars);

    current_patterns_egglog_.clear();
    current_all_vars_.clear();

    int leaf_counter = 0;
    for (auto const &ast : current_patterns_ast_) {
      std::vector<std::string> vars;
      auto egglog_str = pattern_to_egglog(ast, vars, leaf_counter);
      current_patterns_egglog_.push_back(egglog_str);

      for (auto const &v : vars) {
        if (v.size() >= 2 && v[0] == '?' && v[1] == 'v') {
          current_all_vars_.insert(v);
        }
      }
    }
    return true;
  }

  auto match_and_verify_detailed() -> VerifyOutcome {
    if (current_patterns_ast_.empty() || created_ids_.empty()) {
      return VerifyOutcome::SkippedNoPatternOrEmpty;
    }

    egraph_.rebuild(ctx_);

    std::vector<pattern::Pattern<FuzzSymbol>> patterns;
    patterns.reserve(current_patterns_ast_.size());
    for (auto const &ast : current_patterns_ast_) {
      patterns.push_back(pattern_to_memgraph(ast));
    }

    for (auto const &p : patterns) {
      if (p.num_vars() == 0 || p.num_vars() > kMaxPatternVariables) {
        return VerifyOutcome::SkippedUnsupportedPattern;
      }
    }

    auto parse_var_id = [](std::string_view s) -> uint8_t {
      uint8_t id = 0;
      std::from_chars(s.data() + 2, s.data() + s.size(), id);
      return id;
    };

    std::vector<uint8_t> var_ids;
    for (auto const &v : current_all_vars_) {
      if (v.size() >= 3 && v[0] == '?' && v[1] == 'v') {
        var_ids.push_back(parse_var_id(v));
      }
    }
    std::ranges::sort(var_ids);

    using BindingTuple = std::vector<EClassId>;
    std::set<BindingTuple> vm_unique;
    std::size_t vm_raw = 0;

    std::optional<pattern::vm::CompiledMatcher<FuzzSymbol>> compiled_matcher_copy;
    bool vm_compilation_succeeded = false;
    {
      pattern::MatcherIndex matcher(egraph_);
      pattern::vm::VMExecutor vm_executor(egraph_);
      pattern::MatcherContext matcher_ctx;
      std::vector<EClassId> new_eclasses;

      patterns.clear();
      for (auto const &ast : current_patterns_ast_) {
        patterns.push_back(pattern_to_memgraph(ast));
      }

      auto vm_rule_builder = rewrite::RewriteRule<FuzzTypedEGraph>::Builder("fuzz_vm");
      for (auto &p : patterns) {
        vm_rule_builder = std::move(vm_rule_builder).pattern(std::move(p));
      }
      try {
        auto vm_rule = std::move(vm_rule_builder)
                           .apply([this, &vm_raw, &vm_unique, &var_ids](rewrite::RuleContext<FuzzTypedEGraph> &,
                                                                        rewrite::Match const &m) {
                             ++vm_raw;
                             BindingTuple tuple;
                             tuple.reserve(var_ids.size());
                             for (auto id : var_ids) {
                               auto eclass_id = m[pattern::PatternVar{id}];
                               tuple.push_back(egraph_.find(eclass_id));
                             }
                             vm_unique.insert(std::move(tuple));
                           });

        vm_compilation_succeeded = true;
        compiled_matcher_copy = vm_rule.compiled();
        rewrite::RuleContext<FuzzTypedEGraph> rule_ctx(typed_egraph_, new_eclasses);
        vm_rule.match(matcher, vm_executor, matcher_ctx);
        vm_rule.apply(rule_ctx, matcher_ctx);
      } catch (std::overflow_error const &) {
        return VerifyOutcome::SkippedUnsupportedPattern;
      }
    }

    std::size_t const vm_count = vm_unique.size();

    if (opts_.skip_egglog) {
      return VerifyOutcome::SkippedOracleDisabled;
    }

    for (auto const &p : current_patterns_egglog_) {
      if (p.size() >= 2 && p[0] == '?' && p[1] == 'v') {
        return VerifyOutcome::SkippedUnsupportedPattern;
      }
    }

    std::ostringstream full_program;
    full_program << "(datatype Expr\n";
    full_program << "  (A i64)\n  (B i64)\n  (C i64)\n  (D i64)\n  (E i64)\n";
    full_program << "  (F Expr)\n  (G Expr)\n  (H Expr)\n";
    full_program << "  (Plus Expr Expr)\n  (Mul Expr Expr)\n  (Ternary Expr Expr Expr)\n";
    full_program << ")\n\n";
    full_program << egglog_.generate();

    std::vector<std::string> v_vars(current_all_vars_.begin(), current_all_vars_.end());
    std::ranges::sort(v_vars, std::less{}, parse_var_id);

    std::string type_sig;
    std::string var_list;
    for (bool first = true; auto const &v : v_vars) {
      if (!first) {
        type_sig += ' ';
        var_list += ' ';
      }
      type_sig += "Expr";
      var_list += v;
      first = false;
    }

    // Request vm_count + 1 rows so equal counts are never truncated regardless of magnitude,
    // while still bounding reference output to at most one row past the VM count.
    std::size_t const print_limit = vm_count + 1;

    full_program << fmt::format("\n(relation MatchResult ({}))\n", type_sig);
    full_program << "(rule (";
    for (std::size_t i = 0; i < current_patterns_egglog_.size(); ++i) {
      if (i > 0) full_program << " ";
      full_program << fmt::format("(= ?p{} {})", i, current_patterns_egglog_[i]);
    }
    full_program << ")\n";
    full_program << fmt::format("      ((MatchResult {})))\n", var_list);
    full_program << "(run 1000)\n";
    full_program << fmt::format("(print-function MatchResult {})\n", print_limit);

    auto egglog_result = run_egglog(full_program.str(), print_limit, opts_.egglog_bin);
    last_egglog_result_ = egglog_result;

    if (!egglog_result.success) {
      std::cerr << "\n!!! REFERENCE ORACLE TOOL FAILURE !!!\n";
      std::cerr << egglog_result.error << "\n";
      if (opts_.abort_on_failure) {
        std::abort();
      }
      return VerifyOutcome::OracleToolFailure;
    }

    if (egglog_result.truncated && egglog_result.match_count <= vm_count) {
      std::cerr << "\n!!! REFERENCE ORACLE TRUNCATED OUTPUT BEFORE REQUESTED LIMIT !!!\n";
      std::cerr << "Printed " << egglog_result.match_count << " rows for limit " << print_limit << "\n";
      if (opts_.abort_on_failure) {
        std::abort();
      }
      return VerifyOutcome::OracleToolFailure;
    }

    std::size_t const egglog_count = egglog_result.match_count;
    bool const vm_ok = !egglog_result.truncated && (vm_count == egglog_count);

    if (!vm_ok) {
      std::cerr << "\n!!! VM MISMATCH (egglog is ground truth) !!!\n";
      if (egglog_result.truncated) {
        std::cerr << "Egglog:      >=" << egglog_count << " unique matches (truncated at limit " << print_limit
                  << ")\n";
      } else {
        std::cerr << "Egglog:      " << egglog_count << " unique matches (oracle)\n";
      }
      std::cerr << "VM executor: " << vm_count << " unique (" << vm_raw << " raw) WRONG\n";
      dump_egraph(egraph_, std::cerr);
      if (vm_compilation_succeeded && compiled_matcher_copy) {
        std::cerr << "\nVM Bytecode:\n"
                  << pattern::vm::disassemble<FuzzSymbol>(compiled_matcher_copy->code(),
                                                          compiled_matcher_copy->symbols())
                  << "\n";
      }
      if (opts_.abort_on_failure) {
        std::abort();
      }
      return VerifyOutcome::Mismatch;
    }

    ++verified_count_;
    return VerifyOutcome::Verified;
  }

  bool verify_absent() {
    if (created_ids_.empty()) return true;

    static constexpr std::array<FuzzSymbol, 6> kCompoundSymbols = {
        FuzzSymbol::F,
        FuzzSymbol::G,
        FuzzSymbol::H,
        FuzzSymbol::Plus,
        FuzzSymbol::Mul,
        FuzzSymbol::Ternary,
    };

    FuzzSymbol absent_sym{};
    bool found = false;
    for (auto s : kCompoundSymbols) {
      if (!created_symbols_.contains(s)) {
        absent_sym = s;
        found = true;
        break;
      }
    }

    if (!found) {
      return true;
    }

    egraph_.rebuild(ctx_);

    if (!opts_.run_oracle_checks) {
      return true;
    }

    std::size_t arity = 1;
    if (absent_sym == FuzzSymbol::Plus || absent_sym == FuzzSymbol::Mul) arity = 2;
    if (absent_sym == FuzzSymbol::Ternary) arity = 3;

    auto builder = pattern::Pattern<FuzzSymbol>::Builder{};
    utils::small_vector<pattern::PatternNodeId> children;
    for (std::size_t i = 0; i < arity; ++i) {
      children.push_back(builder.var(pattern::PatternVar{static_cast<uint8_t>(i)}));
    }
    builder.sym(absent_sym, std::move(children));
    auto pat = std::move(builder).build();

    std::size_t match_count = 0;
    {
      pattern::MatcherIndex matcher(egraph_);
      pattern::vm::VMExecutor vm_executor(egraph_);
      pattern::MatcherContext matcher_ctx;
      std::vector<EClassId> new_eclasses;

      try {
        auto rule = rewrite::RewriteRule<FuzzTypedEGraph>::Builder("fuzz_absent")
                        .pattern(std::move(pat))
                        .apply([&match_count](rewrite::RuleContext<FuzzTypedEGraph> &, rewrite::Match const &) {
                          ++match_count;
                        });
        rewrite::RuleContext<FuzzTypedEGraph> rule_ctx(typed_egraph_, new_eclasses);
        rule.match(matcher, vm_executor, matcher_ctx);
        rule.apply(rule_ctx, matcher_ctx);
      } catch (std::overflow_error const &) {
        return true;
      }
    }

    if (match_count > 0) {
      std::cerr << "\n!!! VERIFY_ABSENT FAILURE !!!\n";
      std::cerr << "Symbol " << symbol_name(absent_sym) << " was never created but VM found " << match_count
                << " match(es)\n";
      if (opts_.abort_on_failure) {
        std::abort();
      }
      return false;
    }

    return true;
  }

  bool apply_rewrite_cycle(uint8_t const *data, std::size_t &pos, std::size_t size) {
    if (current_patterns_ast_.empty() || created_ids_.empty()) {
      return true;
    }

    uint8_t const shape = (pos < size) ? data[pos++] % 6 : 0;

    egraph_.rebuild(ctx_);

    std::vector<pattern::Pattern<FuzzSymbol>> patterns;
    patterns.reserve(current_patterns_ast_.size());
    for (auto const &ast : current_patterns_ast_) {
      patterns.push_back(pattern_to_memgraph(ast));
    }

    for (auto const &p : patterns) {
      if (p.num_vars() == 0) {
        return true;
      }
    }

    std::vector<EClassId> match_roots;
    {
      pattern::MatcherIndex matcher(egraph_);
      pattern::vm::VMExecutor vm_executor(egraph_);
      pattern::MatcherContext matcher_ctx;
      std::vector<EClassId> new_eclasses;

      auto rule_builder = rewrite::RewriteRule<FuzzTypedEGraph>::Builder("fuzz_rewrite");
      for (auto &p : patterns) {
        rule_builder = std::move(rule_builder).pattern(std::move(p));
      }
      try {
        auto rule =
            std::move(rule_builder)
                .apply([&match_roots](rewrite::RuleContext<FuzzTypedEGraph> &, rewrite::Match const &m) {
                  if (match_roots.size() < 8) {
                    match_roots.push_back(m[pattern::PatternVar{0}]);
                  }
                });
        rewrite::RuleContext<FuzzTypedEGraph> rule_ctx(typed_egraph_, new_eclasses);
        rule.match(matcher, vm_executor, matcher_ctx);
        rule.apply(rule_ctx, matcher_ctx);
      } catch (std::overflow_error const &) {
        return true;
      }
    }

    if (match_roots.empty()) {
      return true;
    }

    for (auto root_id : match_roots) {
      auto canonical = egraph_.find(root_id);
      auto root_name = egglog_names_.contains(canonical) ? egglog_names_[canonical] : egglog_names_[root_id];

      EClassId wrapper_id{};
      std::string wrapper_name;

      switch (shape) {
        case 0:
        case 1:
        case 2: {
          auto sym = static_cast<FuzzSymbol>(10 + shape);
          utils::small_vector<EClassId> children;
          children.push_back(canonical);
          auto info = egraph_.emplace(sym, children);
          wrapper_id = info.eclass_id;
          created_symbols_.insert(sym);
          wrapper_name = egglog_.emit_unary(sym, root_name);
          break;
        }
        case 3:
        case 4: {
          auto sym = (shape == 3) ? FuzzSymbol::Plus : FuzzSymbol::Mul;
          utils::small_vector<EClassId> children;
          children.push_back(canonical);
          children.push_back(canonical);
          auto info = egraph_.emplace(sym, children);
          wrapper_id = info.eclass_id;
          created_symbols_.insert(sym);
          wrapper_name = egglog_.emit_binary(sym, root_name, root_name);
          break;
        }
        case 5: {
          utils::small_vector<EClassId> inner_children;
          inner_children.push_back(canonical);
          auto inner_info = egraph_.emplace(FuzzSymbol::F, inner_children);
          created_ids_.push_back(inner_info.eclass_id);
          auto inner_name = egglog_.emit_unary(FuzzSymbol::F, root_name);
          egglog_names_[inner_info.eclass_id] = inner_name;

          utils::small_vector<EClassId> outer_children;
          outer_children.push_back(inner_info.eclass_id);
          auto outer_info = egraph_.emplace(FuzzSymbol::F, outer_children);
          wrapper_id = outer_info.eclass_id;
          created_symbols_.insert(FuzzSymbol::F);
          wrapper_name = egglog_.emit_unary(FuzzSymbol::F, inner_name);
          break;
        }
        default:
          continue;
      }

      created_ids_.push_back(wrapper_id);
      egglog_names_[wrapper_id] = wrapper_name;

      egraph_.merge(wrapper_id, canonical);
      egglog_.emit_merge(wrapper_name, root_name);
    }

    egraph_.rebuild(ctx_);

    return run_configured_match_check();
  }

  bool finalize() {
    if (created_ids_.empty()) return true;
    egraph_.rebuild(ctx_);
    if (current_patterns_ast_.empty()) return true;
    return run_configured_match_check();
  }

  [[nodiscard]] auto typed_egraph() -> FuzzTypedEGraph & { return typed_egraph_; }
  [[nodiscard]] auto egraph() -> EGraph<FuzzSymbol, FuzzAnalysis> & { return egraph_; }
  [[nodiscard]] auto egraph() const -> EGraph<FuzzSymbol, FuzzAnalysis> const & { return egraph_; }
  [[nodiscard]] auto current_patterns_ast() const -> std::vector<PatternASTPtr> const & {
    return current_patterns_ast_;
  }
  [[nodiscard]] auto created_ids() const -> std::vector<EClassId> const & { return created_ids_; }
  [[nodiscard]] auto created_symbols() const -> std::set<FuzzSymbol> const & { return created_symbols_; }
  [[nodiscard]] auto verified_count() const -> std::size_t { return verified_count_; }
  [[nodiscard]] auto last_egglog_result() const -> EgglogResult const & { return last_egglog_result_; }

 private:
  bool run_configured_match_check() {
    if (!opts_.run_oracle_checks) return true;
    auto const outcome = match_and_verify_detailed();
    return outcome != VerifyOutcome::OracleToolFailure && outcome != VerifyOutcome::Mismatch;
  }

  [[nodiscard]] auto pick_node_idx(uint8_t const *data, std::size_t &pos, std::size_t size) const
      -> std::optional<std::size_t> {
    if (pos + 2 > size || created_ids_.empty()) return std::nullopt;
    auto raw = static_cast<uint16_t>((static_cast<uint16_t>(data[pos]) << 8) | data[pos + 1]);
    pos += 2;
    return static_cast<std::size_t>(raw % created_ids_.size());
  }

  FuzzSessionOptions opts_;
  FuzzTypedEGraph typed_egraph_;
  EGraph<FuzzSymbol, FuzzAnalysis> &egraph_ = typed_egraph_.core();
  ProcessingContext<FuzzSymbol> ctx_;
  std::vector<EClassId> created_ids_;
  std::size_t operation_count_ = 0;
  std::size_t verified_count_ = 0;

  EgglogGenerator egglog_;
  EgglogResult last_egglog_result_;
  std::unordered_map<EClassId, std::string> egglog_names_;
  std::set<FuzzSymbol> created_symbols_;

  MultiPatternGenerator pattern_gen_;
  std::vector<PatternASTPtr> current_patterns_ast_;
  std::vector<std::string> current_patterns_egglog_;
  std::set<uint8_t> current_shared_vars_;
  std::set<std::string> current_all_vars_;
};

}  // namespace memgraph::planner::core::fuzz
