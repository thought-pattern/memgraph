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

#include "query/plan_v2/egraph/symbol_make_traits.hpp"

namespace memgraph::query::plan::v2 {

using enum symbol;

auto symbol_make_traits<Once>::make(storage_type &s) -> seeded_node {
  return {.lowered = {.children = {}, .disambiguator = s.counter++}, .seed = default_analysis_seed<Once>()};
}

auto symbol_make_traits<Symbol>::make(storage_type &s, int32_t pos, std::string_view name, bool user_declared,
                                      query::Symbol::Type type, int32_t token_position) -> seeded_node {
  s.store.try_emplace(pos,
                      SymbolInfo{.name = std::string{name},
                                 .user_declared = user_declared,
                                 .type = type,
                                 .token_position = token_position});
  return {.lowered = {.children = {}, .disambiguator = static_cast<uint64_t>(pos)},
          .seed = default_analysis_seed<Symbol>()};
}

auto symbol_make_traits<Literal>::make(storage_type &s, storage::ExternalPropertyValue const &value) -> seeded_node {
  auto const id = s.values.intern(value, [](auto const &key) { return &key; });

  return {.lowered = {.children = {}, .disambiguator = id},
          .seed = analysis{ExpressionAnalysis{
              .known_constant_value = value,
              .known_length = value.IsAnyList()
                                  ? std::optional{KnownLength{.of = LengthOf::List, .value = value.ListSize()}}
                                  : std::nullopt}}};
}

auto symbol_make_traits<ParamLookup>::make(storage_type & /*s*/, int32_t pos) -> seeded_node {
  return {.lowered = {.children = {}, .disambiguator = static_cast<uint64_t>(pos)},
          .seed = default_analysis_seed<ParamLookup>()};
}

auto symbol_make_traits<Bind>::make(storage_type & /*s*/, planner::core::EClassId input, planner::core::EClassId sym,
                                    planner::core::EClassId expr) -> seeded_node {
  return {.lowered = {.children = utils::small_vector{input, sym, expr}, .disambiguator = std::nullopt},
          .seed = default_analysis_seed<Bind>()};
}

auto symbol_make_traits<Identifier>::make(storage_type & /*s*/, planner::core::EClassId sym) -> seeded_node {
  return {.lowered = {.children = utils::small_vector{sym}, .disambiguator = std::nullopt},
          .seed = default_analysis_seed<Identifier>()};
}

auto symbol_make_traits<Output>::make(storage_type & /*s*/, utils::small_vector<planner::core::EClassId> children)
    -> seeded_node {
  return {.lowered = {.children = std::move(children), .disambiguator = std::nullopt},
          .seed = default_analysis_seed<Output>()};
}

auto symbol_make_traits<NamedOutput>::make(storage_type &s, std::string_view name, planner::core::EClassId sym,
                                           planner::core::EClassId expr) -> seeded_node {
  auto const id = s.names.intern(name, [](auto const &key) { return std::string_view{key}; });
  return {.lowered = {.children = utils::small_vector{sym, expr}, .disambiguator = id},
          .seed = default_analysis_seed<NamedOutput>()};
}

auto symbol_make_traits<Function>::make(storage_type &s, std::string_view name,
                                        utils::small_vector<planner::core::EClassId> args, bool is_pure)
    -> seeded_node {
  auto const id = s.functions.intern(name, [is_pure](auto const &key) {
    return FunctionInfo{.name = key, .kind = BuiltinKindFor(key), .is_pure = is_pure};
  });
  return {.lowered = {.children = std::move(args), .disambiguator = id},
          .seed = analysis{ExpressionAnalysis{.has_impure = !is_pure}}};
}

namespace {

/// The provably-exact length of `range(start, end[, step])` when bounds (and
/// optional step) are statically-known integers and `step != 0` (`nullopt`
/// otherwise). Cypher range is inclusive on both ends; bounds moving opposite
/// the step direction give an empty list.
auto ProvableRangeLength(planner::core::EGraph<symbol, analysis> const &eg,
                         std::span<planner::core::EClassId const> args) -> std::optional<std::size_t> {
  if (args.size() != 2 && args.size() != 3) return std::nullopt;
  auto const a = eg.analysis_of(args[0]).known_int();
  auto const b = eg.analysis_of(args[1]).known_int();
  std::optional<std::int64_t> const step =
      (args.size() == 3) ? eg.analysis_of(args[2]).known_int() : std::optional<std::int64_t>{1};
  if (!a || !b || !step || *step == 0) return std::nullopt;
  if (*step > 0) {
    if (*b < *a) return std::size_t{0};
    auto const span = static_cast<std::uint64_t>(*b) - static_cast<std::uint64_t>(*a);
    auto const ustep = static_cast<std::uint64_t>(*step);
    auto const steps = span / ustep;
    if (steps == UINT64_MAX) return std::nullopt;
    return static_cast<std::size_t>(steps + 1);
  }
  if (*b > *a) return std::size_t{0};
  auto const span = static_cast<std::uint64_t>(*a) - static_cast<std::uint64_t>(*b);
  auto const ustep = std::uint64_t{0} - static_cast<std::uint64_t>(*step);
  auto const steps = span / ustep;
  if (steps == UINT64_MAX) return std::nullopt;
  return static_cast<std::size_t>(steps + 1);
}

}  // namespace

auto symbol_make_traits<Function>::update(planner::core::EGraph<symbol, analysis> const &eg, storage_type const &s,
                                          std::span<planner::core::EClassId const> args, std::uint64_t function_id,
                                          analysis &out) -> bool {
  auto *expr = out.expression_for_update();
  if (expr == nullptr) return false;
  auto const *entry = s.functions.find(function_id);
  if (entry == nullptr) return false;
  auto const &info = *entry;

  bool changed = false;
  if (!expr->has_impure) {
    bool arg_impure = !info.is_pure;
    if (!arg_impure) {
      for (auto arg : args) {
        if (eg.analysis_of(arg).has_impure()) {
          arg_impure = true;
          break;
        }
      }
    }
    if (arg_impure) {
      expr->has_impure = true;
      changed = true;
    }
  }

  // Only a pure function over pure arguments establishes a statically-known fact.
  if (!info.is_pure || expr->has_impure) return changed;

  switch (info.kind) {
    case BuiltinKind::Range: {
      if (expr->known_length) return changed;
      auto const length = ProvableRangeLength(eg, args);
      if (!length) return changed;
      expr->known_length = KnownLength{.of = LengthOf::List, .value = *length};
      return true;
    }
    case BuiltinKind::Size: {
      if (expr->known_constant_value || args.size() != 1) return changed;
      auto const length = eg.analysis_of(args[0]).known_list_length();
      if (!length) return changed;
      expr->known_constant_value = storage::ExternalPropertyValue{static_cast<int64_t>(*length)};
      return true;
    }
    case BuiltinKind::Unknown:
      return changed;
  }
  return changed;
}

auto symbol_make_traits<Add>::update(planner::core::EGraph<symbol, analysis> const &eg, storage_type const & /*s*/,
                                     std::span<planner::core::EClassId const> children, std::uint64_t /*disambiguator*/,
                                     analysis &out) -> bool {
  auto *expr = out.expression_for_update();
  if (expr == nullptr || children.size() != 2) return false;
  bool changed = false;
  if (!expr->has_impure &&
      (eg.analysis_of(children[0]).has_impure() || eg.analysis_of(children[1]).has_impure())) {
    expr->has_impure = true;
    changed = true;
  }
  if (expr->known_length || expr->has_impure) return changed;
  // Both reads answer only for a list's length, so this sums two list lengths
  // rather than inferring list-ness from the presence of a count.
  auto const lhs = eg.analysis_of(children[0]).known_list_length();
  auto const rhs = eg.analysis_of(children[1]).known_list_length();
  if (!lhs || !rhs) return changed;
  expr->known_length = KnownLength{.of = LengthOf::List, .value = *lhs + *rhs};
  return true;
}

auto symbol_make_traits<Unwind>::make(storage_type & /*s*/, planner::core::EClassId input, planner::core::EClassId sym,
                                      planner::core::EClassId list_expr) -> seeded_node {
  return {.lowered = {.children = utils::small_vector{input, sym, list_expr}, .disambiguator = std::nullopt},
          .seed = default_analysis_seed<Unwind>()};
}

auto symbol_make_traits<Subquery>::make(storage_type & /*s*/, utils::small_vector<planner::core::EClassId> children)
    -> seeded_node {
  return {.lowered = {.children = std::move(children), .disambiguator = std::nullopt},
          .seed = default_analysis_seed<Subquery>()};
}

auto symbol_make_traits<Distinct>::make(storage_type & /*s*/, planner::core::EClassId input) -> seeded_node {
  return {.lowered = {.children = utils::small_vector{input}, .disambiguator = std::nullopt},
          .seed = default_analysis_seed<Distinct>()};
}

}  // namespace memgraph::query::plan::v2
