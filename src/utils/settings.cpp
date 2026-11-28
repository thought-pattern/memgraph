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

#include <spdlog/spdlog.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <utility>

#include "utils/exceptions.hpp"
#include "utils/logging.hpp"
#include "utils/settings.hpp"

namespace memgraph::utils {

Settings::Settings(std::filesystem::path storage_path) : storage_(std::move(storage_path)) {}

std::optional<std::string> Settings::RegisterSetting(std::string name, const std::string &default_value,
                                                     Persistence persistence, OnChangeCallback callback,
                                                     Validation validation, Sensitivity sensitivity,
                                                     Normalizer normalizer) {
  const std::scoped_lock settings_guard{settings_lock_};
  MG_ASSERT(
      validation(default_value).has_value(), "\"{}\"'s default value does not satisfy the validation condition.", name);

  const std::string canonical_default = normalizer(default_value);
  auto [it, inserted] = settings_.try_emplace(name,
                                              Entry{.value = canonical_default,
                                                    .persistence = persistence,
                                                    .sensitivity = sensitivity,
                                                    .on_change = std::move(callback),
                                                    .validation = std::move(validation),
                                                    .normalizer = std::move(normalizer)});
  MG_ASSERT(inserted, "Setting '{}' is already registered", name);
  ++revision_;

  auto stored = storage_.GetStrict(name);
  switch (persistence) {
    case Persistence::kPersisted: {
      if (stored.has_value()) {
        it->second.value = it->second.normalizer(*stored);
      } else if (stored.error() == kvstore::KVStore::GetError::kReadError) {
        settings_.erase(it);
        --revision_;
        throw kvstore::KVStoreError("Failed to read persisted setting '{}' during registration", name);
      } else {
        MG_ASSERT(storage_.Put(name, canonical_default), "Failed to register a setting");
      }
      return std::nullopt;
    }
    case Persistence::kRuntimeOnly: {
      if (!stored.has_value()) {
        if (stored.error() == kvstore::KVStore::GetError::kReadError) {
          spdlog::warn("Failed to read leftover stored value for runtime-only setting '{}'", name);
        }
        return std::nullopt;
      }
      // Best-effort cleanup; the value is applied or dropped regardless, so a failed delete only leaves it for the
      // next start
      if (!storage_.Delete(name)) {
        spdlog::warn("Failed to delete the stored value of setting '{}'", name);
      }
      return std::move(*stored);
    }
  }
  return std::nullopt;
}

std::optional<std::string> Settings::GetValue(const std::string &setting_name) const {
  const std::shared_lock settings_guard{settings_lock_};
  const auto it = settings_.find(setting_name);
  if (it == settings_.end()) return std::nullopt;
  return it->second.value;
}

std::optional<std::string> Settings::GetRedactedValue(const std::string &setting_name) const {
  const std::shared_lock settings_guard{settings_lock_};
  const auto it = settings_.find(setting_name);
  if (it == settings_.end()) return std::nullopt;
  if (it->second.sensitivity == Sensitivity::kSecret && !it->second.value.empty()) {
    return std::string{kRedactedValue};
  }
  return it->second.value;
}

uint64_t Settings::CurrentRevision() const {
  const std::shared_lock settings_guard{settings_lock_};
  return revision_;
}

Settings::Snapshot Settings::GetValuesSnapshot(std::span<const std::string_view> setting_names) const {
  const std::shared_lock settings_guard{settings_lock_};
  Snapshot snapshot{.revision = revision_};
  snapshot.values.reserve(setting_names.size());
  for (const auto name : setting_names) {
    const auto it = settings_.find(std::string{name});
    if (it == settings_.end()) {
      snapshot.values.emplace_back(std::nullopt);
    } else {
      snapshot.values.emplace_back(it->second.value);
    }
  }
  return snapshot;
}

bool Settings::SetValue(const std::string &setting_name, const std::string &new_value) {
  const auto settings_change_callback = std::invoke([&, this]() -> std::optional<OnChangeCallback> {
    const std::scoped_lock settings_guard{settings_lock_};
    const auto it = settings_.find(setting_name);
    if (it == settings_.end()) return std::nullopt;

    auto &entry = it->second;
    if (const auto msg = entry.validation(new_value); !msg.has_value()) {
      throw utils::BasicException("Cannot update setting '{}': {}", setting_name, msg.error());
    }

    // Normalize and copy before writing to disk, so persistence and in-memory readers observe the identical canonical
    // representation and a refused allocation cannot leave the new value on disk without updating memory.
    auto value = entry.normalizer(new_value);
    auto on_change = entry.on_change;
    if (entry.persistence == Persistence::kPersisted) {
      MG_ASSERT(storage_.Put(setting_name, value), "Failed to modify the setting");
    }
    entry.value = std::move(value);
    ++revision_;
    return on_change;
  });

  if (!settings_change_callback) {
    return false;
  }

  (*settings_change_callback)();
  return true;
}

bool Settings::SetValueForce(const std::string &setting_name, const std::string &new_value) {
  const std::scoped_lock settings_guard{settings_lock_};
  const auto it = settings_.find(setting_name);
  if (it == settings_.end()) {
    spdlog::error("SetValueForce called for unregistered setting '{}'", setting_name);
    return false;
  }
  auto &entry = it->second;
  auto staged_value = new_value;
  if (entry.persistence == Persistence::kPersisted && !storage_.Put(setting_name, staged_value)) {
    spdlog::error("Failed to force-set setting '{}'", setting_name);
    return false;
  }
  if (entry.value != staged_value) {
    entry.value = std::move(staged_value);
    ++revision_;
  }
  return true;
}

Settings::CasForceStatus Settings::CompareAndSetValuesForce(
    uint64_t expected_revision, std::span<const std::pair<std::string_view, std::string>> updates) {
  const std::scoped_lock settings_guard{settings_lock_};
  if (revision_ != expected_revision) {
    return CasForceStatus::kRevisionMismatch;
  }

  std::vector<std::pair<Entry *, std::string>> staged_memory_updates;
  staged_memory_updates.reserve(updates.size());
  std::map<std::string, std::string> persisted_batch;
  bool any_changed = false;

  for (const auto &[setting_name, new_value] : updates) {
    const auto it = settings_.find(std::string{setting_name});
    if (it == settings_.end()) {
      spdlog::error("CompareAndSetValuesForce called for unregistered setting '{}'", setting_name);
      return CasForceStatus::kUnregisteredSetting;
    }
    if (it->second.value != new_value) {
      any_changed = true;
    }
    staged_memory_updates.emplace_back(&it->second, new_value);
    if (it->second.persistence == Persistence::kPersisted) {
      persisted_batch.emplace(std::string{setting_name}, new_value);
    }
  }

  if (!any_changed) {
    return CasForceStatus::kApplied;
  }

  if (!persisted_batch.empty() && !storage_.PutMultiple(persisted_batch)) {
    spdlog::error("Failed to atomically persist setting batch");
    return CasForceStatus::kStorageError;
  }

  for (auto &[entry_ptr, staged_val] : staged_memory_updates) {
    entry_ptr->value.swap(staged_val);
  }
  ++revision_;
  return CasForceStatus::kApplied;
}

std::vector<std::pair<std::string, std::string>> Settings::AllSettings() const {
  const std::shared_lock settings_guard{settings_lock_};

  std::vector<std::pair<std::string, std::string>> settings;
  settings.reserve(settings_.size());
  for (const auto &[name, entry] : settings_) {
    settings.emplace_back(name, entry.value);
  }
  std::ranges::sort(settings);
  return settings;
}

std::vector<std::pair<std::string, std::string>> Settings::AllSettingsRedacted() const {
  const std::shared_lock settings_guard{settings_lock_};

  std::vector<std::pair<std::string, std::string>> settings;
  settings.reserve(settings_.size());
  for (const auto &[name, entry] : settings_) {
    const std::string visible_value =
        (entry.sensitivity == Sensitivity::kSecret && !entry.value.empty()) ? std::string{kRedactedValue} : entry.value;
    settings.emplace_back(name, visible_value);
  }
  std::ranges::sort(settings);
  return settings;
}

}  // namespace memgraph::utils
