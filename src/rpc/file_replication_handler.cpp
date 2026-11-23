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

#include "rpc/file_replication_handler.hpp"

#include <atomic>
#include <ranges>
#include <system_error>
#include <vector>

#include "flags/general.hpp"
#include "slk/streams.hpp"
#include "utils/logging.hpp"
#include "wire_format/serialization.hpp"

namespace memgraph::rpc {

namespace {

std::atomic<uint64_t> g_next_replication_transfer_id{1};

bool PathHasPrefixComponents(const std::filesystem::path &full, const std::filesystem::path &prefix) {
  auto full_it = full.begin();
  for (auto pref_it = prefix.begin(); pref_it != prefix.end(); ++pref_it, ++full_it) {
    if (full_it == full.end() || *full_it != *pref_it) {
      return false;
    }
  }
  return true;
}

bool ContainsSymlinkAlongRelativeChain(const std::filesystem::path &root, const std::filesystem::path &rel) {
  std::error_code ec;
  if (std::filesystem::is_symlink(root, ec)) {
    return true;
  }
  std::filesystem::path cur = root;
  for (const auto &part : rel) {
    if (part.empty() || part == ".") {
      continue;
    }
    cur /= part;
    const auto st = std::filesystem::symlink_status(cur, ec);
    if (!ec && std::filesystem::is_symlink(st)) {
      return true;
    }
  }
  return false;
}

}  // namespace

FileReplicationHandler::FileReplicationHandler()
    : transfer_subdir_(
          fmt::format("transfer_{}", g_next_replication_transfer_id.fetch_add(1, std::memory_order_relaxed))) {}

FileReplicationHandler::~FileReplicationHandler() {
  if (file_.IsOpen()) {
    // The file will be deleted so I don't care about syncing it before closing
    file_.Close();
  }
  // Nobody should be using paths_ after the FileReplicationHandler gets destructed.
  // A snapshot that was loaded has already been moved into the data directory; whatever is still here wasn't used.
  std::ranges::for_each(paths_ | std::views::filter([](auto const &path) {
                          std::error_code ec;
                          const auto st = std::filesystem::symlink_status(path, ec);
                          return !ec && std::filesystem::exists(st);
                        }),
                        [](auto const &path) { utils::DeleteFile(path); });

  // Remove per-transfer staging subdirectories if empty (std::filesystem::remove never deletes non-empty dirs).
  for (auto it = staging_dirs_.rbegin(); it != staging_dirs_.rend(); ++it) {
    std::error_code ec;
    (void)std::filesystem::remove(*it, ec);
    (void)std::filesystem::remove(it->parent_path(), ec);
  }
}

// The assumption is that the header, request, file name and file size will always fit into the buffer size = 64KiB
// Currently, they are taking few hundred bytes at most so this should be a valid assumption. Also, we aren't expecting
// big growth in message size/
std::optional<size_t> FileReplicationHandler::OpenFile(const uint8_t *data, size_t const size) {
  slk::Reader req_reader(data, size, size);

  auto const maybe_rel_path_str = wire_format::ReadString(&req_reader);
  if (!ValidateFilename(maybe_rel_path_str)) return std::nullopt;

  auto const rel_path = std::filesystem::path{*maybe_rel_path_str};
  if (rel_path.is_absolute() || rel_path.has_root_name() || rel_path.has_root_directory()) {
    spdlog::error("Replication file path must be relative: {}", *maybe_rel_path_str);
    return std::nullopt;
  }

  std::vector<std::string> parts;
  for (const auto &comp : rel_path) {
    const auto s = comp.string();
    if (s.empty() || s == "." || s == ".." || s.contains('/') || s.contains('\\') || s.contains('\0')) {
      spdlog::error("Invalid path component in replication file path: {}", *maybe_rel_path_str);
      return std::nullopt;
    }
    parts.push_back(s);
  }

  std::filesystem::path gp_path;
  std::string file_type;
  std::string filename;
  if (parts.size() == 2) {
    file_type = parts[0];
    filename = parts[1];
  } else if (parts.size() == 4 && parts[0] == "databases") {
    gp_path = std::filesystem::path{"databases"} / parts[1];
    file_type = parts[2];
    filename = parts[3];
  } else {
    spdlog::error("Disallowed replication file path layout: {}", *maybe_rel_path_str);
    return std::nullopt;
  }

  if ((file_type != "snapshots" && file_type != "wal") || filename.empty() || filename.contains('.')) {
    spdlog::error("Disallowed replication file type '{}' or filename '{}'", file_type, filename);
    return std::nullopt;
  }

  std::error_code ec;
  const std::filesystem::path configured_root{FLAGS_data_directory};
  if (!utils::EnsureDir(configured_root)) {
    spdlog::error("Failed to ensure data root directory {}", configured_root.string());
    return std::nullopt;
  }
  const auto canonical_root = std::filesystem::weakly_canonical(configured_root, ec);
  if (ec || canonical_root.empty()) {
    spdlog::error("Failed to canonicalize data root directory {}", configured_root.string());
    return std::nullopt;
  }

  const auto rel_staging = gp_path / "tmp" / transfer_subdir_ / file_type;
  if (ContainsSymlinkAlongRelativeChain(canonical_root, rel_staging)) {
    spdlog::error("Refusing replication staging path containing symlink under {}", canonical_root.string());
    return std::nullopt;
  }

  const auto save_dir = (canonical_root / rel_staging).lexically_normal();
  const auto canonical_save_dir = std::filesystem::weakly_canonical(save_dir, ec);
  if (ec || !PathHasPrefixComponents(canonical_save_dir, canonical_root) || canonical_save_dir == canonical_root) {
    spdlog::error("Replication staging directory {} escapes data root {}", save_dir.string(), canonical_root.string());
    return std::nullopt;
  }

  if (!utils::EnsureDir(canonical_save_dir)) {
    spdlog::error("Failed to create directory {}", canonical_save_dir.string());
    return std::nullopt;
  }
  staging_dirs_.push_back(canonical_save_dir);

  const auto maybe_file_size = wire_format::ReadUint(&req_reader);
  if (!ValidateFileSize(maybe_file_size)) return std::nullopt;

  auto const path = canonical_save_dir / filename;
  const auto existing_st = std::filesystem::symlink_status(path, ec);
  if (!ec && (std::filesystem::exists(existing_st) || std::filesystem::is_symlink(existing_st))) {
    spdlog::error("Replication staging target already exists: {}", path.string());
    return std::nullopt;
  }

  spdlog::trace("Replica will be using file {} with size {}", path, *maybe_file_size);
  if (!file_.Open(path, utils::OutputFile::Mode::OVERWRITE_EXISTING)) {
    spdlog::error("Failed to open file {}. This is not a fatal failure, main will retry the sending of the file.",
                  path);
    return std::nullopt;
  }

  file_size_ = *maybe_file_size;
  written_ = 0;
  paths_.emplace_back(path);

  // First N bytes are file_name and file_size, therefore we don't read full size
  size_t const processed_bytes = req_reader.GetPos();
  auto const written = WriteToFile(data + processed_bytes, size - processed_bytes);
  if (!written) return std::nullopt;
  return processed_bytes + *written;
}

bool FileReplicationHandler::ValidateFilename(std::optional<std::string> const &maybe_filename) {
  if (!maybe_filename) {
    spdlog::error("Filename missing for the received file over the RPC");
    return false;
  }

  auto const &filename = *maybe_filename;
  if (filename.empty()) {
    spdlog::error("Filename is empty");
    return false;
  }

  if (filename.contains('.')) {
    spdlog::error("Filename must not contain extension: {}", filename);
    return false;
  }

  return true;
}

bool FileReplicationHandler::ValidateFileSize(std::optional<uint64_t> const &maybe_filesize) {
  if (!maybe_filesize) {
    spdlog::error("Failed to read file size");
    return false;
  }
  return true;
}

std::expected<size_t, utils::FileWriteError> FileReplicationHandler::WriteToFile(const uint8_t *data,
                                                                                 size_t const size) {
  if (!file_.IsOpen()) {
    return 0;
  }

  size_t processed_bytes{0};
  auto to_write = static_cast<size_t>(std::min<uint64_t>(size, file_size_ - written_));

  auto const fail = [this](utils::FileWriteError const error) {
    // The file being written is always the last one opened; the handle may already be closed.
    spdlog::error("Couldn't write received file {}: {}. The transfer is dropped and main will retry it.",
                  paths_.back(),
                  utils::FileWriteErrorToString(error));
    return std::unexpected{error};
  };

  while (to_write > 0) {
    const auto chunk_size = std::min(to_write, utils::kFileBufferSize);
    if (auto res = file_.Write(data + processed_bytes, chunk_size); !res) return fail(res.error());
    to_write -= chunk_size;
    written_ += chunk_size;
    processed_bytes += chunk_size;
  }

  if (written_ == file_size_) {
    if (auto res = ResetCurrentFile(); !res) return fail(res.error());
  }
  return processed_bytes;
}

std::expected<void, utils::FileWriteError> FileReplicationHandler::ResetCurrentFile() {
  if (!file_.IsOpen()) return {};
  auto synced = file_.Sync();
  file_.Close();
  written_ = 0;
  file_size_ = 0;
  return synced;
}

bool FileReplicationHandler::HasOpenedFile() const { return file_.IsOpen(); }

uint64_t FileReplicationHandler::GetRemainingBytesToWrite() const { return file_size_ - written_; }

const std::vector<std::filesystem::path> &FileReplicationHandler::GetActiveFileNames() const { return paths_; }

}  // namespace memgraph::rpc
