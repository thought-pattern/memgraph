// Copyright 2026 Memgraph Ltd.
//
// Licensed as a Memgraph Enterprise file under the Memgraph Enterprise
// License (the "License"); by using this file, you agree to be bound by the terms of the License, and you may not use
// this file except in compliance with the License. You may obtain a copy of the License at https://memgraph.com/legal.
//
//

#include "auth/rules.hpp"

#include <string>
#include <unordered_set>
#include <utility>

#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include "auth/exceptions.hpp"
#include "utils/string.hpp"

namespace memgraph::auth::rules {

namespace {
nlohmann::json ParseJson(std::string_view str) {
  try {
    return nlohmann::json::parse(str);
  } catch (nlohmann::json::parse_error const &) {
    throw AuthException("Couldn't load auth data!");
  }
}

}  // namespace

nlohmann::json ParseAndMigrateJson(std::string_view str) {
  auto data = ParseJson(str);
  MigrateAuthJson(data);
  return data;
}

void LinkUser(Repository const &repo, User &user) {
  // User set roles on particular databases
  // NOTE Has to be done in this order, otherwise the global roles will overwrite the multi-tenant roles
  // Filled only under MG_ENTERPRISE, below, but read unconditionally when applying the global roles.
  std::unordered_set<std::string> failed_mt_roles;  // NOLINT(misc-const-correctness)
#ifdef MG_ENTERPRISE
  auto mt_link = repo.Get(Repository::MtLinkKey(user.username()));
  if (mt_link) {
    try {
      auto json_data = ParseJson(*mt_link);
      if (!json_data.is_object()) {
        spdlog::warn("Found invalid JSON in mtlink format for user '{}'", user.username());
        return;
      }
      for (const auto &[db, roles_array] : json_data.items()) {
        if (!roles_array.is_array()) {
          spdlog::warn(
              "Invalid mtlink entry for user '{}': expected array of rolenames for db '{}'", user.username(), db);
          continue;
        }
        for (const auto &rolename_json : roles_array) {
          if (!rolename_json.is_string()) {
            spdlog::warn(
                "Invalid mtlink entry for user '{}': expected string rolename for db '{}'", user.username(), db);
            continue;
          }
          const auto &rolename = rolename_json.get<std::string>();
          auto role = GetRole(repo, rolename);
          if (!role) {
            spdlog::warn("Role '{}' doesn't exist for user '{}'", rolename, user.username());
            continue;
          }
          try {
            user.AddMultiTenantRole(*role, db);
          } catch (const AuthException &e) {
            spdlog::warn("Couldn't add multi-tenant role '{}' to user '{}' on database '{}': {}",
                         rolename,
                         user.username(),
                         db,
                         e.what());
            failed_mt_roles.insert(rolename);
          }
        }
      }
    } catch (const AuthException &) {
      spdlog::warn("Found invalid JSON in mtlink format for user '{}'", user.username());
    } catch (const nlohmann::detail::exception &) {
      // This shouldn't happen after V2 migration, but handle gracefully
      spdlog::warn("Found invalid JSON in mtlink format for user '{}'", user.username());
    }
  }
#endif

  // User set these roles on all databases
  auto link = repo.Get(Repository::RoleLinkKey(user.username()));
  if (link) {
    try {
      // Parse as JSON array (V2 format)
      auto json_data = ParseJson(*link);
      if (!json_data.is_array()) {
        spdlog::warn("Found invalid JSON in link format for user '{}'", user.username());
        return;
      }
      // V2 format: array of role names
      for (const auto &role_name : json_data) {
        if (role_name.is_string()) {
          // Check that the role is not already added (via the multi-tenant role) or failed to add
          if (failed_mt_roles.contains(role_name.get<std::string>()) ||
              user.roles().GetRole(role_name.get<std::string>())) {
            continue;
          }
          auto role = GetRole(repo, role_name.get<std::string>());
          if (role) {
            user.AddRole(*role);
          }
        }
      }

    } catch (const AuthException &) {
      spdlog::warn("Found invalid JSON in link format for user '{}'", user.username());
      return;
    } catch (const nlohmann::detail::exception &) {
      // This shouldn't happen after V2 migration, but handle gracefully
      spdlog::warn("Found invalid JSON in link format for user '{}'", user.username());
      return;
    }
  }

#ifdef MG_ENTERPRISE
  // Profile linking moved to UserProfiles class
#endif
}

void LinkUserStrict(Repository const &repo, User &user) {
#ifdef MG_ENTERPRISE
  auto mt_link = repo.Get(Repository::MtLinkKey(user.username()));
  if (mt_link) {
    auto json_data = ParseJson(*mt_link);
    if (!json_data.is_object()) {
      throw AuthException("Invalid multi-tenant role link for account '{}'.", user.username());
    }
    for (const auto &[db, roles_array] : json_data.items()) {
      if (!roles_array.is_array()) {
        throw AuthException("Invalid multi-tenant role list on database '{}' for account '{}'.", db, user.username());
      }
      for (const auto &rolename_json : roles_array) {
        if (!rolename_json.is_string()) {
          throw AuthException("Invalid multi-tenant role entry on database '{}' for account '{}'.", db, user.username());
        }
        const auto rolename = rolename_json.get<std::string>();
        auto role = GetRole(repo, rolename);
        if (!role) {
          throw AuthException("Missing role '{}' referenced in multi-tenant link for account '{}'.",
                              rolename,
                              user.username());
        }
        user.AddMultiTenantRole(*role, db);
      }
    }
  }
#endif

  auto link = repo.Get(Repository::RoleLinkKey(user.username()));
  if (link) {
    auto json_data = ParseJson(*link);
    if (!json_data.is_array()) {
      throw AuthException("Invalid role link format for account '{}'.", user.username());
    }
    for (const auto &role_name : json_data) {
      if (!role_name.is_string()) {
        throw AuthException("Invalid role entry in role link for account '{}'.", user.username());
      }
      const auto rolename = role_name.get<std::string>();
      if (user.roles().GetRole(rolename)) {
        continue;
      }
      auto role = GetRole(repo, rolename);
      if (!role) {
        throw AuthException("Missing role '{}' referenced in role link for account '{}'.", rolename, user.username());
      }
      user.AddRole(*role);
    }
  }
}

std::optional<User> GetUser(Repository const &repo, std::string_view username_raw) {
  auto username = utils::ToLowerCase(username_raw);
  auto existing_user = repo.Get(Repository::UserKey(username));
  if (!existing_user) return std::nullopt;

  auto user = User::Deserialize(ParseAndMigrateJson(*existing_user));
  LinkUser(repo, user);
  return user;
}

std::optional<Role> GetRole(Repository const &repo, std::string_view rolename_raw) {
  auto rolename = utils::ToLowerCase(rolename_raw);
  auto existing_role = repo.Get(Repository::RoleKey(rolename));
  if (!existing_role) return std::nullopt;

  auto role = Role::Deserialize(ParseAndMigrateJson(*existing_role));
  return role;
}

std::vector<User> AllUsers(Repository const &repo) {
  std::vector<User> ret;
  repo.ForEachUser([&](auto username, auto const &value) {
    if (username != utils::ToLowerCase(username)) return;
    try {
      User user = User::Deserialize(ParseAndMigrateJson(value));
      LinkUser(repo, user);
      ret.emplace_back(std::move(user));
    } catch (AuthException &e) {
      // One unreadable record must not fail the whole listing.
      spdlog::warn("Skipping user '{}': {}", username, e.what());
    }
  });
  return ret;
}

std::vector<User> AllUsersStrict(Repository const &repo) {
  std::vector<User> ret;
  repo.ForEachUser([&](auto username, auto const &value) {
    if (username != utils::ToLowerCase(username)) {
      throw AuthException("Encountered non-normalized account key '{}' during strict recovery snapshot.", username);
    }
    User user = User::Deserialize(ParseAndMigrateJson(value));
    LinkUserStrict(repo, user);
    ret.emplace_back(std::move(user));
  });
  return ret;
}

std::vector<std::string> AllUsernames(Repository const &repo) {
  std::vector<std::string> ret;
  repo.ForEachUser([&](auto username, auto const &value) {
    if (username != utils::ToLowerCase(username)) return;
    try {
      User::Deserialize(ParseAndMigrateJson(value));
      ret.emplace_back(std::move(username));
    } catch (AuthException &e) {
      // One unreadable record must not fail the whole listing.
      spdlog::warn("Skipping user '{}': {}", username, e.what());
    }
  });
  return ret;
}

// Unlike the listings around it, an unreadable record here is fatal rather than skipped. This feeds
// SystemRecoveryRpc, so a skipped role would leave a replica recovering without a role the main has, and its
// permission decisions would diverge silently. A failed recovery is the better outcome.
//
// AllUsers goes into that same RPC and skips instead. That is not a considered difference, only the older
// behaviour kept: the argument above applies to a user just as well. Making both fatal is the open question.
std::vector<Role> AllRoles(Repository const &repo) {
  std::vector<Role> ret;
  repo.ForEachRole([&](auto rolename, auto const &value) {
    if (rolename != utils::ToLowerCase(rolename)) return;
    Role role = Role::Deserialize(ParseAndMigrateJson(value));
    ret.emplace_back(std::move(role));
  });
  return ret;
}

std::vector<std::string> AllRolenames(Repository const &repo) {
  std::vector<std::string> ret;
  repo.ForEachRole([&](auto rolename, auto const &value) {
    if (rolename != utils::ToLowerCase(rolename)) return;
    try {
      Role::Deserialize(ParseAndMigrateJson(value));
      ret.emplace_back(std::move(rolename));
    } catch (AuthException &e) {
      // One unreadable record must not fail the whole listing.
      spdlog::warn("Skipping role '{}': {}", rolename, e.what());
    }
  });
  return ret;
}

namespace {
bool ParseRoleLinkMembership(std::string const &username, std::string const &value, std::string const &rolename,
                             std::vector<std::string> &unreadable_entries, bool &complete) {
  auto mark_unreadable = [&](std::string_view reason) {
    spdlog::warn("Unreadable role assignment for '{}': {}", username, reason);
    unreadable_entries.push_back(username);
    complete = false;
  };

  try {
    auto json_data = ParseJson(value);
    if (!json_data.is_array()) {
      mark_unreadable("expected JSON array of role names");
      return false;
    }
    bool matched = false;
    bool malformed_element = false;
    for (auto const &role_name : json_data) {
      if (!role_name.is_string()) {
        malformed_element = true;
        continue;
      }
      if (utils::ToLowerCase(role_name.template get<std::string>()) == rolename) {
        matched = true;
      }
    }
    if (malformed_element) {
      mark_unreadable("role assignment contains a non-string entry");
    }
    return matched;
  } catch (AuthException const &e) {
    mark_unreadable(e.what());
    return false;
  } catch (nlohmann::detail::exception const &e) {
    mark_unreadable(e.what());
    return false;
  }
}
}  // namespace

RoleMembersListing ListUsersForRole(Repository const &repo, std::string_view rolename_raw) {
  const auto rolename = utils::ToLowerCase(rolename_raw);
  RoleMembersListing listing;
  repo.ForEachRoleLink([&](auto username, auto const &value) {
    if (username != utils::ToLowerCase(username)) return;
    if (!ParseRoleLinkMembership(username, value, rolename, listing.unreadable_entries, listing.complete)) {
      return;
    }
    try {
      if (auto user = GetUser(repo, username)) {
        listing.users.push_back(std::move(*user));
      } else {
        spdlog::warn("Role assignment for '{}' references a missing account record.", username);
        listing.unreadable_entries.push_back(username);
        listing.complete = false;
      }
    } catch (AuthException const &e) {
      spdlog::warn("Skipping unreadable account '{}' while listing role '{}': {}", username, rolename, e.what());
      listing.unreadable_entries.push_back(username);
      listing.complete = false;
    }
  });
  return listing;
}

RoleMemberNamesListing ListUsernamesForRole(Repository const &repo, std::string_view rolename_raw) {
  const auto rolename = utils::ToLowerCase(rolename_raw);
  RoleMemberNamesListing listing;
  repo.ForEachRoleLink([&](auto username, auto const &value) {
    if (username != utils::ToLowerCase(username)) return;
    if (ParseRoleLinkMembership(username, value, rolename, listing.unreadable_entries, listing.complete)) {
      listing.usernames.push_back(std::move(username));
    }
  });
  return listing;
}

std::vector<User> AllUsersForRole(Repository const &repo, std::string_view rolename_raw) {
  return ListUsersForRole(repo, rolename_raw).users;
}

std::vector<std::string> AllUsernamesForRole(Repository const &repo, std::string_view rolename_raw) {
  return ListUsernamesForRole(repo, rolename_raw).usernames;
}

bool HasUser(Repository const &repo, std::string_view name) {
  auto username = utils::ToLowerCase(name);
  return repo.Get(Repository::UserKey(username)).has_value();
}

bool HasRole(Repository const &repo, std::string_view name) {
  auto rolename = utils::ToLowerCase(name);
  return repo.Get(Repository::RoleKey(rolename)).has_value();
}

bool HasUsers(Repository const &repo) { return repo.HasAnyUser(); }

}  // namespace memgraph::auth::rules
