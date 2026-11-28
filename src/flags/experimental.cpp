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

#include <fmt/format.h>
#include <gflags/gflags.h>
#include <spdlog/spdlog.h>
#include <cctype>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "flags/experimental.hpp"
#include "utils/flag_validation.hpp"
#include "utils/string.hpp"

using namespace std::string_view_literals;

// The mapping tables MUST be defined before DEFINE_VALIDATED_string so the
// help-string composition below sees them initialised.  Static-init order
// within a single TU is source order for dynamically-initialised objects;
// gflags' FlagRegisterer (constructed by DEFINE_VALIDATED_string further
// down) reads description as `const char*` at construction, so we point it
// at a static std::string built once from the mappings.
namespace memgraph::flags {

auto const mapping = std::map{
    std::pair{"planner-v2"sv, Experiments::PLANNER_V2},
    std::pair{"commit-lock-narrowing"sv, Experiments::COMMIT_LOCK_NARROWING},
};
auto const reverse_mapping = std::map{
    std::pair{Experiments::PLANNER_V2, "planner-v2"sv},
    std::pair{Experiments::COMMIT_LOCK_NARROWING, "commit-lock-narrowing"sv},
};
auto const config_mapping = std::map<std::string_view, Experiments>{};

}  // namespace memgraph::flags

namespace {

auto CanonicalizeExperimentToken(std::string_view raw_token) -> std::string {
  return memgraph::utils::ToLowerCase(memgraph::utils::Trim(raw_token));
}

auto SplitCommaSeparatedTokens(std::string_view raw_list) -> std::vector<std::string> {
  std::vector<std::string> tokens;
  if (raw_list.empty()) {
    return tokens;
  }
  size_t start = 0;
  while (true) {
    const auto comma = raw_list.find(',', start);
    if (comma == std::string_view::npos) {
      tokens.push_back(CanonicalizeExperimentToken(raw_list.substr(start)));
      break;
    }
    tokens.push_back(CanonicalizeExperimentToken(raw_list.substr(start, comma - start)));
    start = comma + 1;
  }
  return tokens;
}

/// Compose "prefix Options [name1, name2, ...]" from a mapping table.
/// Used to keep --help text in sync with the actual recognised options
/// without hand-maintaining the list at every DEFINE_*.
template <typename Mapping>
auto BuildHelp(std::string_view prefix, Mapping const &m) -> std::string {
  std::string out{prefix};
  out += " Options [";
  bool first = true;
  for (auto const &[name, _] : m) {
    if (!first) out += ", ";
    first = false;
    out.append(name.data(), name.size());
  }
  out += "]";
  return out;
}

// Static lifetime ⇒ pointer is valid for the program's lifetime, which is
// the gflags registry's only requirement on description strings.
std::string const kEnabledHelp =
    BuildHelp("Experimental features to be used, comma-separated.", memgraph::flags::mapping);
std::string const kConfigHelp =
    BuildHelp("Experimental features to be used, JSON object.", memgraph::flags::config_mapping);

}  // namespace

// NOLINTNEXTLINE (cppcoreguidelines-avoid-non-const-global-variables)
DEFINE_VALIDATED_string(experimental_enabled, "", kEnabledHelp.c_str(),
                        { return memgraph::flags::ValidExperimentalFlag(value); });

// NOLINTNEXTLINE (cppcoreguidelines-avoid-non-const-global-variables)
DEFINE_VALIDATED_string(experimental_config, "", kConfigHelp.c_str(),
                        { return memgraph::flags::ValidExperimentalConfig(value); });

namespace memgraph::flags {

auto ExperimentsInstance() -> Experiments & {
  static auto instance = Experiments{};
  return instance;
}

bool AreExperimentsEnabled(Experiments experiments) {
  auto actual = std::to_underlying(ExperimentsInstance());
  auto check = std::to_underlying(experiments);

  return (actual & check) == check;
}

auto ReadExperimental(std::string const &flags_experimental) -> Experiments {
  auto const mapping_end = mapping.cend();
  using underlying_type = std::underlying_type_t<Experiments>;
  auto to_set = underlying_type{};

  for (auto const &experiment : SplitCommaSeparatedTokens(flags_experimental)) {
    if (auto it = mapping.find(experiment); it != mapping_end) {
      spdlog::info(fmt::format("Experimental feature {} is enabled.", it->first));
      to_set |= std::to_underlying(it->second);
    }
  }

  return static_cast<Experiments>(to_set);
}

void SetExperimental(Experiments const &experiments) { ExperimentsInstance() = experiments; }

void AppendExperimental(Experiments const &experiments) {
  using underlying_type = std::underlying_type_t<Experiments>;
  auto current_state = std::to_underlying(ExperimentsInstance());
  auto new_experiments = std::to_underlying(experiments);
  auto to_set = underlying_type{};
  to_set |= current_state;
  to_set |= new_experiments;

  SetExperimental(static_cast<Experiments>(to_set));
}

auto ValidExperimentalFlag(std::string_view value) -> bool {
  if (value.empty()) {
    return true;
  }
  auto const mapping_end = mapping.cend();
  for (auto const &experiment : SplitCommaSeparatedTokens(value)) {
    if (mapping.find(experiment) == mapping_end) {
      return false;
    }
  }
  return true;
}

auto ValidExperimentalConfig(std::string_view json_config) -> bool {
  if (json_config.empty()) {
    return true;
  }

  try {
    auto json_flags = nlohmann::json::parse(json_config);
    if (!json_flags.is_object()) {
      return false;
    }

    auto const config_mapping_end = config_mapping.cend();
    for (auto const &[key, _] : json_flags.items()) {
      auto const canonical_key = CanonicalizeExperimentToken(key);
      if (config_mapping.find(canonical_key) == config_mapping_end) {
        return false;
      }
    }
  } catch (nlohmann::json::parse_error const &e) {
    return false;
  }

  return true;
}

auto ParseExperimentalConfig(Experiments experiment) -> nlohmann::json {
  const auto &json_config = FLAGS_experimental_config;

  if (json_config.empty()) {
    return nlohmann::json::object();
  }

  try {
    auto json_flags = nlohmann::json::parse(json_config);
    if (!json_flags.is_object()) {
      throw std::invalid_argument("Experimental config must be a JSON object.");
    }

    auto mapping_it = reverse_mapping.find(experiment);
    if (mapping_it == reverse_mapping.end()) {
      throw std::invalid_argument("Unknown experimental feature in experimental config.");
    }

    auto experiment_json_config = json_flags.find(mapping_it->second);
    if (experiment_json_config == json_flags.end()) {
      throw std::invalid_argument("Experimental feature configuration missing in JSON.");
    }

    return *experiment_json_config;

  } catch (const nlohmann::json::parse_error &e) {
    throw std::invalid_argument("Invalid experimental config: " + std::string(e.what()));
  }
}

}  // namespace memgraph::flags
