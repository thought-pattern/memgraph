// Copyright 2026 Memgraph Ltd.
//
// Licensed as a Memgraph Enterprise file under the Memgraph Enterprise
// License (the "License"); by using this file, you agree to be bound by the terms of the License, and you may not use
// this file except in compliance with the License. You may obtain a copy of the License at https://memgraph.com/legal.
//
//

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "auth/models.hpp"
#include "auth/repository.hpp"

namespace memgraph::auth::rules {

/// Auth's read model, as plain functions over a repository.
///
/// Each takes the store to read from, so none of them can tell whether they are reading durable state or a
/// transaction's buffered overlay. Nothing here touches auth modules, the permission-cache epoch, resource
/// monitoring or replication: those are side effects, and they stay with `Auth`.

/// Parses a stored user or role record and brings it up to the current format.
nlohmann::json ParseAndMigrateJson(std::string_view str);

struct RoleMembersListing {
  std::vector<User> users;
  std::vector<std::string> unreadable_entries;
  bool complete{true};
};

struct RoleMemberNamesListing {
  std::vector<std::string> usernames;
  std::vector<std::string> unreadable_entries;
  bool complete{true};
};

/// Resolves a user's roles, per-database then global, and attaches them. Reads the link entries.
void LinkUser(Repository const &repo, User &user);

/// Resolves a user's roles strictly, throwing AuthException if any role link or referenced role is unreadable.
void LinkUserStrict(Repository const &repo, User &user);

std::optional<User> GetUser(Repository const &repo, std::string_view username);

std::optional<Role> GetRole(Repository const &repo, std::string_view rolename);

std::vector<User> AllUsers(Repository const &repo);

/// Enumerates all users strictly for system recovery, failing if any user record or role link is unreadable.
std::vector<User> AllUsersStrict(Repository const &repo);

std::vector<std::string> AllUsernames(Repository const &repo);

std::vector<Role> AllRoles(Repository const &repo);

std::vector<std::string> AllRolenames(Repository const &repo);

RoleMembersListing ListUsersForRole(Repository const &repo, std::string_view rolename);

RoleMemberNamesListing ListUsernamesForRole(Repository const &repo, std::string_view rolename);

std::vector<User> AllUsersForRole(Repository const &repo, std::string_view rolename);

std::vector<std::string> AllUsernamesForRole(Repository const &repo, std::string_view rolename);

bool HasUser(Repository const &repo, std::string_view username);

bool HasRole(Repository const &repo, std::string_view rolename);

bool HasUsers(Repository const &repo);

}  // namespace memgraph::auth::rules
