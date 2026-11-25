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

#include "build_info.hpp"

#ifdef __linux__
#include <link.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace memgraph::utils {

namespace {

struct ElfNoteHeader {
  uint32_t n_namesz;
  uint32_t n_descsz;
  uint32_t n_type;
};

constexpr uint32_t kNtGnuBuildId = 3U;

// dl_iterate_phdr is not async-signal-safe; this only runs at startup and from
// the terminate handler (normal context), never from a signal handler.
std::string ComputeBuildId() {
#ifdef __linux__
  std::string result;
  dl_iterate_phdr(
      [](dl_phdr_info *info, size_t /*size*/, void *data) -> int {
        auto *out = static_cast<std::string *>(data);
        for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
          const ElfW(Phdr) &ph = info->dlpi_phdr[i];
          if (ph.p_type != PT_NOTE) continue;
          // dlpi_addr + p_vaddr is the segment's runtime address; the int-to-ptr cast is intrinsic to ELF.
          // NOLINTNEXTLINE(performance-no-int-to-ptr)
          const auto *p = reinterpret_cast<const unsigned char *>(info->dlpi_addr + ph.p_vaddr);
          auto parsed = ParseElfNoteSegmentBuildId(std::span<const unsigned char>{p, ph.p_memsz});
          if (!parsed.empty()) {
            *out = std::move(parsed);
            return 1;
          }
        }
        return 1;  // only inspect the main executable (the first object)
      },
      &result);
  return result;
#else
  return {};
#endif
}

}  // namespace

std::string ParseElfNoteSegmentBuildId(std::span<const unsigned char> segment) {
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::size_t offset = 0;
  while (segment.size() - offset >= sizeof(ElfNoteHeader)) {
    ElfNoteHeader hdr{};
    std::memcpy(&hdr, segment.data() + offset, sizeof(hdr));
    const std::size_t header_end = offset + sizeof(ElfNoteHeader);
    const std::size_t remaining_after_hdr = segment.size() - header_end;
    if (hdr.n_namesz > remaining_after_hdr) {
      break;
    }
    const std::size_t name_pad = (static_cast<std::size_t>(hdr.n_namesz) + 3U) & ~std::size_t{3U};
    if (name_pad > remaining_after_hdr) {
      break;
    }
    const std::size_t remaining_after_name = remaining_after_hdr - name_pad;
    if (hdr.n_descsz > remaining_after_name) {
      break;
    }

    const char *name = reinterpret_cast<const char *>(segment.data() + header_end);
    const unsigned char *desc = segment.data() + header_end + name_pad;
    if (hdr.n_type == kNtGnuBuildId && hdr.n_namesz == 4 && std::memcmp(name, "GNU\0", 4) == 0) {
      std::string out;
      out.reserve(static_cast<std::size_t>(hdr.n_descsz) * 2);
      for (uint32_t j = 0; j < hdr.n_descsz; ++j) {
        out.push_back(kHex[desc[j] >> 4]);
        out.push_back(kHex[desc[j] & 0x0FU]);
      }
      return out;
    }

    const std::size_t desc_pad = (static_cast<std::size_t>(hdr.n_descsz) + 3U) & ~std::size_t{3U};
    if (desc_pad > remaining_after_name) {
      break;
    }
    offset = header_end + name_pad + desc_pad;
  }
  return {};
}

std::string GetBuildId() { return ComputeBuildId(); }

BuildInfo GetBuildInfo() {
#ifdef CMAKE_BUILD_TYPE_NAME
  constexpr const char *build_info_name = CMAKE_BUILD_TYPE_NAME;
#else
  constexpr const char *build_info_name = "unknown";
#endif
#ifdef MEMGRAPH_VERSION_STRING
  constexpr const char *build_version = MEMGRAPH_VERSION_STRING;
#else
  constexpr const char *build_version = "unknown";
#endif
  return BuildInfo{.build_name = build_info_name, .version = build_version, .build_id = ComputeBuildId()};
}

}  // namespace memgraph::utils
