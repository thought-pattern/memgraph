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

#include <fmt/format.h>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "mg_procedure.h"
#include "query/procedure/fmt.hpp"
#include "utils/logging.hpp"

namespace memgraph::query::procedure {
template <typename TResult, typename TFunc, typename... TArgs>
TResult Call(TFunc func, TArgs... args) {
  static_assert(std::is_trivially_copyable_v<TFunc>);
  static_assert((std::is_trivially_copyable_v<std::remove_reference_t<TArgs>> && ...));
  TResult result{};
  MG_ASSERT(func(args..., &result) == mgp_error::MGP_ERROR_NO_ERROR);
  return result;
}

template <typename TFunc, typename... TArgs>
bool CallBool(TFunc func, TArgs... args) {
  return Call<int>(func, args...) != 0;
}

template <typename TObj>
using MgpRawObjectDeleter = void (*)(TObj *);

template <typename TObj>
using MgpUniquePtr = std::unique_ptr<TObj, MgpRawObjectDeleter<TObj>>;

template <typename TObj, typename TFunc, typename... TArgs>
mgp_error CreateMgpObject(MgpUniquePtr<TObj> &obj, TFunc func, TArgs &&...args) {
  TObj *raw_obj{nullptr};
  const auto err = func(std::forward<TArgs>(args)..., &raw_obj);
  obj.reset(raw_obj);
  return err;
}

template <typename Fun>
[[nodiscard]] bool TryOrSetError(Fun &&func, mgp_result *result) {
  if (const auto err = func(); err == mgp_error::MGP_ERROR_UNABLE_TO_ALLOCATE) {
    static_cast<void>(mgp_result_set_error_msg(result, "Not enough memory!"));
    return false;
  } else if (err != mgp_error::MGP_ERROR_NO_ERROR) {
    const auto error_msg = fmt::format("Unexpected error ({})!", err);
    static_cast<void>(mgp_result_set_error_msg(result, error_msg.c_str()));
    return false;
  }
  return true;
}

[[nodiscard]] MgpUniquePtr<mgp_value> GetStringValueOrSetError(const char *string, mgp_memory *memory,
                                                               mgp_result *result);

[[nodiscard]] bool InsertResultOrSetError(mgp_result *result, mgp_result_record *record, const char *result_name,
                                          mgp_value *value);

// Extracts the `data.<property>` fields a specified-property text query references when every clause in the
// query is explicitly scoped to a `data.<property>` field. Quoted literals and regex literals are skipped so
// field-like text inside search values is ignored, and any unqualified clause, wildcard/non-`data.` field, or
// malformed group/quote returns `std::nullopt` so the caller falls back to the index's full property set.
inline std::optional<std::vector<std::string_view>> TryExtractCompleteTextQueryFields(std::string_view query) {
  constexpr std::string_view kFieldPrefix = "data.";
  const auto is_ws = [](const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
  const auto is_ident = [](const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  };
  const auto is_boundary = [&](const size_t idx) {
    if (idx >= query.size()) return true;
    const char c = query[idx];
    return is_ws(c) || c == '(' || c == ')';
  };
  const auto skip_quoted_or_delim = [&](size_t &pos, const char closer) -> bool {
    ++pos;  // skip opening delimiter
    while (pos < query.size()) {
      if (query[pos] == '\\') {
        pos += 2;
        continue;
      }
      if (query[pos] == closer) {
        ++pos;
        return true;
      }
      ++pos;
    }
    return false;
  };
  const auto skip_term_modifiers = [&](size_t &pos) {
    while (pos < query.size() && (query[pos] == '~' || query[pos] == '^')) {
      ++pos;
      while (pos < query.size() && !is_ws(query[pos]) && query[pos] != '(' && query[pos] != ')') {
        ++pos;
      }
    }
  };
  const auto consume_atom_value = [&](size_t &pos) -> bool {
    if (pos >= query.size()) return false;
    const char ch = query[pos];
    if (ch == '"' || ch == '\'' || ch == '/') {
      if (!skip_quoted_or_delim(pos, ch)) return false;
      skip_term_modifiers(pos);
      return true;
    }
    if (ch == '[' || ch == '{') {
      const char closer = (ch == '[') ? ']' : '}';
      if (!skip_quoted_or_delim(pos, closer)) return false;
      skip_term_modifiers(pos);
      return true;
    }
    if (ch == ')' || is_ws(ch)) return false;
    while (pos < query.size() && !is_ws(query[pos]) && query[pos] != '(' && query[pos] != ')') {
      if (query[pos] == '\\') {
        pos += 2;
        continue;
      }
      if (query[pos] == '"' || query[pos] == '\'' || query[pos] == '/') {
        const char q = query[pos];
        if (!skip_quoted_or_delim(pos, q)) return false;
        continue;
      }
      ++pos;
    }
    return true;
  };

  std::vector<std::string_view> fields;
  std::vector<bool> group_qualified_stack;
  bool inherited_qualified = false;
  size_t pos = 0;

  while (pos < query.size()) {
    while (pos < query.size() && is_ws(query[pos])) ++pos;
    if (pos >= query.size()) break;

    if (query[pos] == ')') {
      if (group_qualified_stack.empty()) return std::nullopt;
      inherited_qualified = group_qualified_stack.back();
      group_qualified_stack.pop_back();
      ++pos;
      skip_term_modifiers(pos);
      continue;
    }

    while (pos < query.size() && (query[pos] == '+' || query[pos] == '-')) {
      ++pos;
      if (pos >= query.size() || is_ws(query[pos])) return std::nullopt;
    }

    if ((query.substr(pos).starts_with("AND") && is_boundary(pos + 3)) ||
        (query.substr(pos).starts_with("NOT") && is_boundary(pos + 3))) {
      pos += 3;
      continue;
    }
    if (query.substr(pos).starts_with("OR") && is_boundary(pos + 2)) {
      pos += 2;
      continue;
    }

    if (query[pos] == '(') {
      group_qualified_stack.push_back(inherited_qualified);
      ++pos;
      continue;
    }

    // Check whether this atom begins with an unescaped `<field>:` prefix.
    size_t scan = pos;
    while (scan < query.size() && (is_ident(query[scan]) || query[scan] == '.' || query[scan] == '*')) {
      ++scan;
    }
    if (scan > pos && scan < query.size() && query[scan] == ':') {
      const auto field_spec = query.substr(pos, scan - pos);
      if (!field_spec.starts_with(kFieldPrefix)) return std::nullopt;
      const auto prop_name = field_spec.substr(kFieldPrefix.size());
      if (prop_name.empty()) return std::nullopt;
      for (const char c : prop_name) {
        if (!is_ident(c)) return std::nullopt;
      }
      fields.push_back(prop_name);
      pos = scan + 1;
      while (pos < query.size() && is_ws(query[pos])) ++pos;
      if (pos >= query.size()) return std::nullopt;
      while (pos < query.size() && (query[pos] == '+' || query[pos] == '-')) {
        ++pos;
        if (pos >= query.size() || is_ws(query[pos])) return std::nullopt;
      }
      if (query[pos] == '(') {
        group_qualified_stack.push_back(inherited_qualified);
        inherited_qualified = true;
        ++pos;
        continue;
      }
      if (!consume_atom_value(pos)) return std::nullopt;
      continue;
    }

    if (!inherited_qualified) {
      return std::nullopt;
    }
    if (!consume_atom_value(pos)) return std::nullopt;
  }

  if (!group_qualified_stack.empty() || fields.empty()) {
    return std::nullopt;
  }
  return fields;
}
}  // namespace memgraph::query::procedure
