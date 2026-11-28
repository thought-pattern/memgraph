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

// Fuzzer for VM pattern build, compile, and matching.
// Verifies match counts against egglog as an oracle using private per-run
// temporary input files and synchronized environment initialization.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

#include "fuzz_pattern_corpus.hpp"

namespace memgraph::planner::core {

namespace {
std::once_flag g_init_once;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)
fuzz::FuzzSessionOptions g_default_opts;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void init_fuzz_options() {
  auto env_flag = [](char const *name) -> bool {
    char const *val = std::getenv(name);
    if (val == nullptr) return false;
    std::string_view const sv{val};
    return sv == "1" || sv == "true";
  };

  g_default_opts.verbose = env_flag("FUZZ_VERBOSE");
  g_default_opts.skip_egglog = env_flag("FUZZ_SKIP_EGGLOG");
  g_default_opts.run_oracle_checks = true;
  g_default_opts.abort_on_failure = true;
  if (char const *bin = std::getenv("FUZZ_EGGLOG_BIN"); bin != nullptr && bin[0] != '\0') {
    g_default_opts.egglog_bin = bin;
  }
  if (g_default_opts.skip_egglog) {
    std::cerr << "fuzz_pattern_vm: FUZZ_SKIP_EGGLOG enabled; reference oracle verification skipped\n";
  }
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const *data, size_t size) {
  if (size < 4) return 0;

  std::call_once(g_init_once, init_fuzz_options);

  fuzz::FuzzPatternSession session{g_default_opts};
  if (!session.replay_stream(data, size)) {
    std::abort();
  }
  if (!session.finalize()) {
    std::abort();
  }

  return 0;
}

}  // namespace memgraph::planner::core
