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

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gflags/gflags.h>

#include "utils/logging.hpp"

/// Reads the memgraph configuration files.
///
/// Load flags in this order, the last one has the highest priority:
///   1) /etc/memgraph/memgraph.conf
///   2) ~/.memgraph/config
///   3) env - MEMGRAPH_CONFIG
inline void LoadConfig(const std::string &product_name) {
  namespace fs = std::filesystem;
  std::vector<fs::path> configs = {fs::path("/etc/memgraph/memgraph.conf")};
  if (getenv("HOME") != nullptr) configs.emplace_back(fs::path(getenv("HOME")) / fs::path(".memgraph/config"));
  {
    auto *memgraph_config = getenv("MEMGRAPH_CONFIG");
    if (memgraph_config != nullptr) {
      auto path = fs::path(memgraph_config);
      MG_ASSERT(
          fs::exists(path), "MEMGRAPH_CONFIG environment variable set to nonexisting path: {}", path.generic_string());
      configs.emplace_back(path);
    }
  }

  std::vector<std::string> flagfile_arguments;
  for (const auto &config : configs)
    if (fs::exists(config)) {
      flagfile_arguments.emplace_back(std::string("--flag-file=" + config.generic_string()));
    }

  int custom_argc = static_cast<int>(flagfile_arguments.size()) + 1;
  auto custom_argv = std::make_unique<char *[]>(custom_argc);

  custom_argv[0] = strdup(product_name.c_str());
  for (int i = 0; i < static_cast<int>(flagfile_arguments.size()); ++i) {
    custom_argv[i + 1] = strdup(flagfile_arguments[i].c_str());
  }

  // setup flags from config flags
  auto *argv_ptr = custom_argv.get();
  gflags::ParseCommandLineFlags(&custom_argc, &argv_ptr, false);

  // unconsumed arguments have to be freed to avoid memory leak since they are
  // strdup-ed.
  for (int i = 0; i < custom_argc; ++i) free(custom_argv[i]);
}

inline std::pair<std::string, std::string> LoadUsernameAndPassword(const std::string &pass_file) {
  std::ifstream file(pass_file);
  if (!file.is_open()) {
    throw std::runtime_error("Unable to open MG_PASSFILE: " + pass_file);
  }

  std::string line;
  if (!std::getline(file, line)) {
    throw std::runtime_error("MG_PASSFILE is empty or unreadable: " + pass_file);
  }
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }

  std::vector<std::string> fields;
  std::string token;
  for (std::size_t idx = 0; idx < line.size(); ++idx) {
    if (line[idx] == '\\' && idx + 1 < line.size() && line[idx + 1] == ':') {
      token.push_back(':');
      ++idx;
    } else if (line[idx] == ':') {
      fields.push_back(std::move(token));
      token.clear();
    } else {
      token.push_back(line[idx]);
    }
  }
  fields.push_back(std::move(token));

  if (fields.size() != 2 || fields[0].empty() || fields[1].empty()) {
    throw std::runtime_error("Invalid MG_PASSFILE format; expected non-empty username:password");
  }

  return {std::move(fields[0]), std::move(fields[1])};
}

inline std::optional<uint16_t> ParseBoltPortOverride(std::string_view raw) noexcept {
  if (raw.empty()) {
    return std::nullopt;
  }
  uint32_t parsed = 0;
  const auto *begin = raw.data();
  const auto *end = raw.data() + raw.size();
  const auto [ptr, ec] = std::from_chars(begin, end, parsed);
  if (ec != std::errc{} || ptr != end || parsed > std::numeric_limits<uint16_t>::max()) {
    return std::nullopt;
  }
  return static_cast<uint16_t>(parsed);
}
