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

#include "auth/profiles/user_profiles.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "slk/serialization.hpp"
#include "spdlog/spdlog.h"

#include "auth/exceptions.hpp"
#include "auth/rpc.hpp"
#include "utils/string.hpp"

namespace {

std::string FoldUsername(std::string_view username) { return memgraph::utils::ToLowerCase(username); }

}  // namespace

namespace memgraph::auth {

void to_json(nlohmann::json &data, const memgraph::auth::UserProfiles::limits_t &limits) {
  using up = memgraph::auth::UserProfiles;
  for (const auto &[key, val] : limits) {
    if (std::holds_alternative<up::unlimitted_t>(val)) {
      data.emplace(up::kLimits[(size_t)key], nlohmann::json(/*null*/));
    } else {
      data.emplace(up::kLimits[(size_t)key], std::get<uint64_t>(val));
    }
  }
}

void from_json(const nlohmann::json &data, memgraph::auth::UserProfiles::limits_t &limits) {
  using up = memgraph::auth::UserProfiles;
  if (!data.is_object()) {
    throw AuthException("Invalid resource profile limits: expected JSON object.");
  }
  limits.clear();
  for (const auto &[key, val] : data.items()) {
    const auto *key_it = std::ranges::find(up::kLimits, key);
    if (key_it == up::kLimits.end()) {
      throw AuthException("Unknown resource profile limit '{}'.", key);
    }
    const auto key_idx = std::distance(up::kLimits.begin(), key_it);
    if (val.is_null()) {
      limits[(up::Limits)key_idx] = up::unlimitted_t{};
    } else if (val.is_number_unsigned()) {
      limits[(up::Limits)key_idx] = val.get<uint64_t>();
    } else if (val.is_number_integer()) {
      const auto signed_val = val.get<int64_t>();
      if (signed_val < 0) {
        throw AuthException("Negative value for resource profile limit '{}'.", key);
      }
      limits[(up::Limits)key_idx] = static_cast<uint64_t>(signed_val);
    } else {
      throw AuthException("Invalid value type for resource profile limit '{}'.", key);
    }
  }
}

void to_json(nlohmann::json &data, const memgraph::auth::UserProfiles::Profile &profile) {
  data["limits"] = profile.limits;
  data["usernames"] = profile.usernames;
}

void from_json(const nlohmann::json &data, memgraph::auth::UserProfiles::Profile &profile) {
  if (!data.is_object()) {
    throw AuthException("Invalid resource profile record: expected JSON object.");
  }
  memgraph::auth::UserProfiles::limits_t parsed_limits;
  if (auto lim_it = data.find("limits"); lim_it != data.end()) {
    from_json(*lim_it, parsed_limits);
  }
  std::unordered_set<std::string> parsed_usernames;
  if (auto usr_it = data.find("usernames"); usr_it != data.end()) {
    if (!usr_it->is_array()) {
      throw AuthException("Invalid resource profile membership list: expected JSON array.");
    }
    // Fold on load so a raw spelling persisted by a pre-fix binary stays matchable without a durable rewrite.
    for (const auto &entry : *usr_it) {
      if (!entry.is_string()) {
        throw AuthException("Invalid resource profile member entry: expected string.");
      }
      parsed_usernames.insert(FoldUsername(entry.get<std::string>()));
    }
  }
  profile.limits = std::move(parsed_limits);
  profile.usernames = std::move(parsed_usernames);
}

UserProfiles::UserProfiles(Repository &durability) : durability_{&durability} {
  // No migration at the moment
  if (!durability_->Put(kUserProfilesVersionKey, kUserProfilesVersion)) {
    spdlog::error("Failed to put user profiles version");
  }

  // Per folded user: (raw spelling, profile name).
  std::map<std::string, std::vector<std::pair<std::string, std::string>>> spellings;

  // Populate local storage
  durability_->ForEachProfile([this, &spellings](auto const &name, auto const &value) {
    try {
      const auto data = nlohmann::json::parse(value);
      auto profile = data.template get<memgraph::auth::UserProfiles::Profile>();
      profile.name = name;
      if (auto usr_it = data.find("usernames"); usr_it != data.end()) {
        for (const auto &raw_json : *usr_it) {
          auto raw = raw_json.template get<std::string>();
          spellings[FoldUsername(raw)].emplace_back(std::move(raw), name);
        }
      }
      profiles_.emplace(std::move(profile));
    } catch (const AuthException &e) {
      spdlog::error("Failed to load resource profile '{}': {}", name, e.what());
    } catch (const nlohmann::json::exception &e) {
      spdlog::error("Failed to parse resource profile '{}': {}", name, e.what());
    }
  });

  // Pre-fold data can put spellings of one user in several profiles; keep the one with the largest raw spelling
  // (all-lowercase when present, as older binaries enforced; ties: smallest profile name). Durable data is untouched.
  for (const auto &[user, entries] : spellings) {
    std::set<std::string> names;
    const std::pair<std::string, std::string> *winner = nullptr;
    for (const auto &entry : entries) {
      names.insert(entry.second);
      if (winner == nullptr || entry.first > winner->first ||
          (entry.first == winner->first && entry.second < winner->second)) {
        winner = &entry;
      }
    }
    if (names.size() < 2) {
      continue;
    }
    for (const auto &[raw, name] : entries) {
      if (name != winner->second) {
        profiles_.find(name)->usernames.erase(user);
        stale_[name].push_back(raw);
      }
    }
    spdlog::error(
        "User '{}' is assigned to profiles {} under different spellings; applying profile '{}'. Run SET PROFILE FOR {} "
        "TO <profile> to choose a profile, or CLEAR PROFILE FOR {} to remove it.",
        user,
        utils::Join(names, ", "),
        winner->second,
        user,
        user);
  }
};

std::string UserProfiles::SerializeWithStale(const Profile &profile, const std::vector<std::string> *stale_spellings) {
  nlohmann::json json = profile;
  if (stale_spellings != nullptr) {
    for (const auto &raw : *stale_spellings) {
      json["usernames"].push_back(raw);
    }
  }
  return json.dump();
}

bool UserProfiles::Persist(const Profile &profile) {
  const auto it = stale_.find(profile.name);
  const std::vector<std::string> *stale_ptr = (it != stale_.end()) ? &it->second : nullptr;
  return durability_->Put(Repository::ProfileKey(profile.name), SerializeWithStale(profile, stale_ptr));
}

void UserProfiles::Resolve(const std::string &folded) {
  for (auto it = stale_.begin(); it != stale_.end();) {
    auto &spellings = it->second;
    const auto saved = spellings;
    if (std::erase_if(spellings, [&](const auto &raw) { return FoldUsername(raw) == folded; }) != 0) {
      if (const auto profile_it = profiles_.find(it->first); profile_it != profiles_.end() && !Persist(*profile_it)) {
        spellings = saved;
        spdlog::warn("Failed to clear stale profile membership of user '{}' in profile '{}'.", folded, it->first);
      }
    }
    it = spellings.empty() ? stale_.erase(it) : std::next(it);
  }
}

bool UserProfiles::Create(std::string_view name, limits_t defined_limits,
                          const std::unordered_set<std::string> &usernames) {
  auto l = std::unique_lock{mtx_};
  if (profiles_.contains(name)) {
    return false;
  }

  std::unordered_set<std::string> folded_usernames;
  folded_usernames.reserve(usernames.size());
  for (const auto &username : usernames) {
    folded_usernames.insert(FoldUsername(username));
  }
  Profile profile{std::string{name}, std::move(defined_limits), std::move(folded_usernames)};

  auto next_stale = stale_;
  std::unordered_set<std::string> stale_touched_profiles;
  for (auto st_it = next_stale.begin(); st_it != next_stale.end();) {
    const auto erased = std::erase_if(
        st_it->second, [&](const auto &raw) { return profile.usernames.contains(FoldUsername(raw)); });
    if (erased > 0) {
      stale_touched_profiles.insert(st_it->first);
    }
    st_it = st_it->second.empty() ? next_stale.erase(st_it) : std::next(st_it);
  }

  std::map<std::string, std::string> batch_puts;
  for (const auto &existing_profile : profiles_) {
    bool modified = stale_touched_profiles.contains(existing_profile.name);
    for (const auto &username : profile.usernames) {
      if (existing_profile.usernames.contains(username)) {
        modified = true;
        break;
      }
    }
    if (!modified) continue;

    Profile updated_existing = existing_profile;
    for (const auto &username : profile.usernames) {
      updated_existing.usernames.erase(username);
    }
    const auto st_it = next_stale.find(updated_existing.name);
    const std::vector<std::string> *st_ptr = (st_it != next_stale.end()) ? &st_it->second : nullptr;
    batch_puts.emplace(Repository::ProfileKey(updated_existing.name), SerializeWithStale(updated_existing, st_ptr));
  }
  batch_puts.emplace(Repository::ProfileKey(profile.name), SerializeWithStale(profile, nullptr));

  if (!durability_->PutMultiple(batch_puts)) {
    throw AuthException("Failed to persist resource profile '{}'!", name);
  }

  for (auto &existing_profile : profiles_) {  // NOLINT
    for (const auto &username : profile.usernames) {
      existing_profile.usernames.erase(username);
    }
  }
  stale_ = std::move(next_stale);
  profiles_.emplace(std::move(profile));
  return true;
}

std::optional<UserProfiles::Profile> UserProfiles::Update(std::string_view name, const limits_t &updated_limits) {
  auto l = std::unique_lock{mtx_};
  auto profile_it = profiles_.find(name);
  if (profile_it == profiles_.end()) {
    return std::nullopt;
  }
  Profile updated = *profile_it;
  for (const auto &[key, val] : updated_limits) {
    updated.limits[key] = val;
  }
  if (!Persist(updated)) {
    throw AuthException("Failed to persist resource profile '{}'!", name);
  }
  profile_it->limits = std::move(updated.limits);
  return *profile_it;
}

UserProfiles::DropResult UserProfiles::Drop(std::string_view name) {
  auto l = std::unique_lock{mtx_};
  auto profile_it = profiles_.find(name);
  if (profile_it == profiles_.end()) {
    if (durability_->Get(Repository::ProfileKey(name))) {
      return durability_->Delete(Repository::ProfileKey(name)) ? DropResult::kDropped : DropResult::kFailed;
    }
    return DropResult::kAbsent;
  }
  if (!durability_->Delete(Repository::ProfileKey(name))) {
    return DropResult::kFailed;
  }
  auto old_profile = *profile_it;
  profiles_.erase(profile_it);
  stale_.erase(old_profile.name);
  for (const auto &username : old_profile.usernames) {
    Resolve(username);
  }
  return DropResult::kDropped;
}

std::optional<UserProfiles::Profile> UserProfiles::Get(std::string_view name) const {
  auto l = std::shared_lock{mtx_};
  auto profile_it = profiles_.find(name);
  if (profile_it == profiles_.end()) {
    return std::nullopt;
  }
  return *profile_it;
}

std::vector<UserProfiles::Profile> UserProfiles::GetAll() const {
  auto l = std::shared_lock{mtx_};
  std::vector<UserProfiles::Profile> profiles;
  profiles.reserve(profiles_.size());
  for (const auto &profile : profiles_) {
    profiles.emplace_back(profile);
  }
  return profiles;
}

std::optional<UserProfiles::Profile> UserProfiles::AddUsername(std::string_view profile_name,
                                                               std::string_view username) {
  auto l = std::unique_lock{mtx_};
  auto profile_it = profiles_.find(profile_name);
  if (profile_it == profiles_.end()) {
    return std::nullopt;
  }

  const auto folded_username = FoldUsername(username);

  auto next_stale = stale_;
  std::unordered_set<std::string> stale_touched_profiles;
  for (auto st_it = next_stale.begin(); st_it != next_stale.end();) {
    const auto erased =
        std::erase_if(st_it->second, [&](const auto &raw) { return FoldUsername(raw) == folded_username; });
    if (erased > 0) {
      stale_touched_profiles.insert(st_it->first);
    }
    st_it = st_it->second.empty() ? next_stale.erase(st_it) : std::next(st_it);
  }

  std::map<std::string, std::string> batch_puts;
  for (const auto &other : profiles_) {
    if (other.name == profile_it->name) continue;
    if (!other.usernames.contains(folded_username) && !stale_touched_profiles.contains(other.name)) continue;
    Profile updated_other = other;
    updated_other.usernames.erase(folded_username);
    const auto st_it = next_stale.find(updated_other.name);
    const std::vector<std::string> *st_ptr = (st_it != next_stale.end()) ? &st_it->second : nullptr;
    batch_puts.emplace(Repository::ProfileKey(updated_other.name), SerializeWithStale(updated_other, st_ptr));
  }

  Profile updated_target = *profile_it;
  updated_target.usernames.insert(folded_username);
  {
    const auto st_it = next_stale.find(updated_target.name);
    const std::vector<std::string> *st_ptr = (st_it != next_stale.end()) ? &st_it->second : nullptr;
    batch_puts.emplace(Repository::ProfileKey(updated_target.name), SerializeWithStale(updated_target, st_ptr));
  }

  if (!durability_->PutMultiple(batch_puts)) {
    throw AuthException("Failed to persist profile '{}' for '{}'!", profile_name, username);
  }

  for (auto &other : profiles_) {  // NOLINT
    if (other.name != profile_it->name) {
      other.usernames.erase(folded_username);
    }
  }
  profile_it->usernames.insert(folded_username);
  stale_ = std::move(next_stale);
  return *profile_it;
}

UserProfiles::MembershipResult UserProfiles::RemoveUsername(std::string_view profile_name, std::string_view username) {
  auto l = std::unique_lock{mtx_};
  auto profile_it = profiles_.find(profile_name);
  if (profile_it == profiles_.end()) {
    return MembershipResult::kAbsent;
  }

  const auto folded_username = FoldUsername(username);
  auto username_it = profile_it->usernames.find(folded_username);
  if (username_it == profile_it->usernames.end()) {
    return MembershipResult::kAbsent;
  }

  profile_it->usernames.erase(username_it);

  // Update durability
  if (!Persist(*profile_it)) {
    // Revert changes
    profile_it->usernames.insert(folded_username);
    return MembershipResult::kFailed;
  }

  Resolve(folded_username);
  return MembershipResult::kChanged;
}

std::unordered_set<std::string> UserProfiles::GetUsernames(std::string_view profile_name) const {
  auto l = std::shared_lock{mtx_};
  auto profile_it = profiles_.find(profile_name);
  if (profile_it == profiles_.end()) {
    return {};
  }

  return profile_it->usernames;
}

std::optional<std::string> UserProfiles::GetProfileForUsername(std::string_view username) const {
  auto l = std::shared_lock{mtx_};
  const auto folded_username = FoldUsername(username);
  for (const auto &profile : profiles_) {
    if (profile.usernames.contains(folded_username)) {
      return profile.name;
    }
  }
  return std::nullopt;
}

bool UserProfiles::IsAmbiguous(std::string_view username) const {
  auto l = std::shared_lock{mtx_};
  const auto folded = FoldUsername(username);
  return std::ranges::any_of(stale_, [&](const auto &entry) {
    return std::ranges::any_of(entry.second, [&](const auto &raw) { return FoldUsername(raw) == folded; });
  });
}

std::optional<UserProfiles::Profile> UserProfiles::Merge(const std::optional<Profile> &user,
                                                         const std::optional<Profile> &role) {
  if (!user && !role) return std::nullopt;
  if (!user) return role;
  if (!role) return user;
  Profile merged = *user;
  merged.name = fmt::format("{}\n{}", user->name, role->name);
  for (const auto &[limit, value] : role->limits) {
    const auto [it, succ] = merged.limits.try_emplace(limit, value);
    if (!succ) {
      // Limit already present, merge (take the lower value)
      if (std::holds_alternative<unlimitted_t>(it->second)) {
        // If the existing limit is unlimited, we overwrite it with the role's limit
        it->second = value;
      } else if (std::holds_alternative<unlimitted_t>(value)) {
        // If the role's limit is unlimited, we keep the existing one
        continue;
      } else {
        // Otherwise, we keep the lower value
        it->second = std::min(std::get<uint64_t>(it->second), std::get<uint64_t>(value));
      }
    }
  }
  return merged;
}

UserProfiles::MemorySnapshot UserProfiles::CaptureMemorySnapshot() const {
  const std::shared_lock lk(mtx_);
  return MemorySnapshot{.profiles = std::vector<Profile>{profiles_.begin(), profiles_.end()}, .stale = stale_};
}

void UserProfiles::RestoreMemorySnapshot(MemorySnapshot snapshot) {
  const std::unique_lock lk(mtx_);
  profiles_.clear();
  for (auto &profile : snapshot.profiles) {
    profiles_.insert(std::move(profile));
  }
  stale_ = std::move(snapshot.stale);
}

}  // namespace memgraph::auth

namespace memgraph::slk {
// JSON needs to/from in the same namespace
void to_json(nlohmann::json &data, const memgraph::auth::UserProfiles::limits_t &limits) {
  memgraph::auth::to_json(data, limits);
}

void from_json(const nlohmann::json &data, memgraph::auth::UserProfiles::limits_t &limits) {
  memgraph::auth::from_json(data, limits);
}

// Serialize code for auth::UserProfiles::Profile
void Save(const auth::UserProfiles::Profile &self, memgraph::slk::Builder *builder) {
  nlohmann::json json;
  json[self.name] = self;
  memgraph::slk::Save(json.dump(), builder);
}

// Deserialize code for auth::UserProfiles::Profile
void Load(auth::UserProfiles::Profile *self, memgraph::slk::Reader *reader) {
  std::string tmp;
  memgraph::slk::Load(&tmp, reader);
  const auto json = nlohmann::json::parse(tmp);
  const auto name = json.begin().key();
  auto profile = json.begin().value().get<auth::UserProfiles::Profile>();
  profile.name = name;
  *self = std::move(profile);
}
}  // namespace memgraph::slk
