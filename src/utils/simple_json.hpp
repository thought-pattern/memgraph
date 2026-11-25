// Copyright 2024 Memgraph Ltd.
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

#include <cctype>
#include <cmath>
#include <iosfwd>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <type_traits>

namespace memgraph::utils {

static inline std::regex GetRegexKey(const std::string &key) {
  return std::regex("\"" + key + "\"\\s*:\\s*\"([^\"]+)\"|\"" + key + "\"\\s*:\\s*([^,\\}]+)");
}

// Function to parse JSON string and return a value by key as a string.
static inline bool GetJsonValue(const std::string &jsonString, const std::string &key, std::string &out) {
  const auto regex = GetRegexKey(key);
  std::smatch match;

  if (std::regex_search(jsonString, match, regex) && match[1].matched) {
    out = match[1].str();
    return true;
  }
  return false;
}

// Overloaded GetJsonValue function for different types.
template <typename T>
static inline bool GetJsonValue(const std::string &jsonString, const std::string &key, T &out) {
  const auto regex = GetRegexKey(key);
  std::smatch match;

  if (std::regex_search(jsonString, match, regex)) {
    for (size_t i = 1; i < match.size(); ++i) {
      if (!match[i].matched) continue;
      const auto m = match[i].str();
      size_t start = 0;
      while (start < m.size() && std::isspace(static_cast<unsigned char>(m[start])) != 0) {
        ++start;
      }
      if (start == m.size()) return false;
      if constexpr (std::is_unsigned_v<T> && !std::is_same_v<T, bool>) {
        if (m[start] == '-') return false;
      }
      std::istringstream ss(m);
      T parsed{};
      if (!(ss >> parsed)) return false;
      ss >> std::ws;
      if (!ss.eof()) return false;
      if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(parsed)) return false;
      }
      out = parsed;
      return true;
    }
  }
  return false;
}

};  // namespace memgraph::utils
