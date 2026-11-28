// Copyright 2023 Memgraph Ltd.
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

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include "flags/scheduler.hpp"
#include "gflags/gflags.h"

// Bolt server flags.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DECLARE_string(bolt_address);
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DECLARE_int32(bolt_port);
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DECLARE_int32(bolt_num_workers);
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DECLARE_string(bolt_cert_file);
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DECLARE_string(bolt_key_file);
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
// DECLARE_string(bolt_server_name_for_init); Moved to run_time_configurable

namespace memgraph::flags {

inline constexpr int32_t kMaxPriorityQueueBoltWorkers = 1024;

std::expected<void, std::string> CheckBoltTLSFlags(std::string_view cert_file, std::string_view key_file);
void ValidateBoltTLSFlags();

std::expected<void, std::string> CheckBoltWorkersForScheduler(int32_t bolt_num_workers, SchedulerType scheduler_type);
void ValidateBoltWorkerFlags();

}  // namespace memgraph::flags
