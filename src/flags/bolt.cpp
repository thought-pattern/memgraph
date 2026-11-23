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

#include "flags/bolt.hpp"

#include <fmt/format.h>
#include <gflags/gflags.h>
#include <algorithm>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <string_view>

#include "utils/flag_validation.hpp"
#include "utils/logging.hpp"
#include "utils/priority_thread_pool.hpp"
#include "utils/system_info.hpp"

static_assert(memgraph::flags::kMaxPriorityQueueBoltWorkers ==
              static_cast<int32_t>(memgraph::utils::HotMask::kMaxElements));

// Bolt server flags.
DEFINE_string(bolt_address, "0.0.0.0", "IP address on which the Bolt server should listen.");

DEFINE_VALIDATED_int32(bolt_port, 7687, "Port on which the Bolt server should listen.",
                       FLAG_IN_RANGE(0, std::numeric_limits<uint16_t>::max()));

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DEFINE_VALIDATED_int32(bolt_num_workers, memgraph::utils::GetSafeHardwareConcurrency(),
                       "Number of workers used by the Bolt server. By default, this will be the "
                       "number of processing units available on the machine.",
                       FLAG_IN_RANGE(1, INT32_MAX));

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DEFINE_string(bolt_cert_file, "", "Certificate file which should be used for the Bolt server.");
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DEFINE_string(bolt_key_file, "", "Key file which should be used for the Bolt server.");

namespace memgraph::flags {

std::expected<void, std::string> CheckBoltTLSFlags(std::string_view cert_file, std::string_view key_file) {
  if (cert_file.empty() != key_file.empty()) {
    return std::unexpected{fmt::format(
        "Bolt TLS requires --bolt-cert-file and --bolt-key-file to be set together (or both to be empty). "
        "Refusing to start a plaintext Bolt listener with an incomplete TLS configuration. "
        "cert_file=\"{}\" key_file=\"{}\"",
        cert_file,
        key_file)};
  }
  return {};
}

void ValidateBoltTLSFlags() {
  if (const auto res = CheckBoltTLSFlags(FLAGS_bolt_cert_file, FLAGS_bolt_key_file); !res.has_value()) {
    LOG_FATAL("{}", res.error());
  }
}

std::expected<void, std::string> CheckBoltWorkersForScheduler(int32_t bolt_num_workers, SchedulerType scheduler_type) {
  if (bolt_num_workers < 1) {
    return std::unexpected{fmt::format("--bolt-num-workers must be at least 1 (got {}).", bolt_num_workers)};
  }
  if (scheduler_type == SchedulerType::PRIORITY_QUEUE_WITH_SIDECAR &&
      bolt_num_workers > kMaxPriorityQueueBoltWorkers) {
    return std::unexpected{fmt::format(
        "--bolt-num-workers={} exceeds the maximum {} supported by the priority_queue scheduler.",
        bolt_num_workers,
        kMaxPriorityQueueBoltWorkers)};
  }
  return {};
}

void ValidateBoltWorkerFlags() {
  if (const auto res = CheckBoltWorkersForScheduler(FLAGS_bolt_num_workers, GetSchedulerType()); !res.has_value()) {
    LOG_FATAL("{}", res.error());
  }
}

}  // namespace memgraph::flags
