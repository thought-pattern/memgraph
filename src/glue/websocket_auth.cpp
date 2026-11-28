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

#include "glue/websocket_auth.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "auth/exceptions.hpp"
#include "dbms/constants.hpp"
#include "utils/variant_helpers.hpp"

namespace memgraph::glue {

namespace {

bool RefreshCachedPrincipal(const auth::Auth &locked_auth, std::optional<auth::UserOrRole> &principal) {
  if (!principal) {
    return false;
  }
  bool valid = true;
  std::visit(utils::Overloaded{[&](auth::User &user) {
                                 auto tmp = locked_auth.GetUser(user.username());
                                 if (!tmp) {
                                   valid = false;
                                   return;
                                 }
                                 user = std::move(*tmp);
                               },
                               [&](auth::Roles &roles) {
                                 auth::Roles updated_roles;
                                 for (const auto &role : roles) {
                                   auto tmp = locked_auth.GetRole(role.rolename());
                                   if (tmp) {
                                     updated_roles.AddRole(*tmp);
                                   } else {
                                     valid = false;
                                     return;
                                   }
                                 }
                                 if (updated_roles.rolenames().empty()) {
                                   valid = false;
                                   return;
                                 }
                                 roles = std::move(updated_roles);
                               }},
             *principal);
  if (!valid) {
    principal.reset();
    return false;
  }
  return true;
}

bool PrincipalHasWebsocketPermission(const auth::UserOrRole &principal) {
  return std::visit(utils::Overloaded{[](const auto &entity) {
#ifdef MG_ENTERPRISE
                      std::optional<std::string> db_name;
                      try {
                        const auto &main_db = entity.GetMain();
                        if (!main_db.empty()) db_name = main_db;
                      } catch (const auth::AuthException &) {
                        db_name = std::nullopt;
                      }
                      const std::optional<std::string_view> db_view =
                          db_name ? std::optional<std::string_view>{*db_name} : std::nullopt;
                      return entity.GetPermissions(db_view).Has(auth::Permission::WEBSOCKET) ==
                             auth::PermissionLevel::GRANT;
#else
                      return entity.GetPermissions(dbms::kDefaultDB).Has(auth::Permission::WEBSOCKET) ==
                             auth::PermissionLevel::GRANT;
#endif
                    }},
                    principal);
}

class PerSessionSafeAuth final : public communication::websocket::AuthenticationInterface::SessionAuth {
 public:
  explicit PerSessionSafeAuth(auth::SynchedAuth *auth) : auth_(auth) {}

  bool Authenticate(const std::string &username, const std::string &password) override {
    dropped_ = false;
    principal_ = auth::Authenticate(*auth_, username, password);
    signed_in_ = principal_.has_value();
    if (!signed_in_) {
      return false;
    }
    auto locked_auth = auth_->ReadLock();
    (void)locked_auth->UpToDate(auth_epoch_);
    return true;
  }

  bool IsAuthorized() override {
    auto locked_auth = auth_->ReadLock();
    if (dropped_) {
      return false;
    }
    if (!signed_in_) {
      return !locked_auth->AccessControlled();
    }
    if (!principal_) {
      return false;
    }
    if (!locked_auth->UpToDate(auth_epoch_)) {
      if (!RefreshCachedPrincipal(*locked_auth, principal_)) {
        dropped_ = true;
        return false;
      }
    }
#ifdef MG_ENTERPRISE
    return PrincipalHasWebsocketPermission(*principal_);
#else
    return true;
#endif
  }

 private:
  auth::SynchedAuth *auth_;
  std::optional<auth::UserOrRole> principal_{};
  auth::Auth::Epoch auth_epoch_{};
  bool signed_in_{false};
  bool dropped_{false};
};

}  // namespace

bool SafeAuth::Authenticate(const std::string &username, const std::string &password) const {
  user_or_role_ = auth::Authenticate(*auth_, username, password);
  if (user_or_role_) {
    auto locked_auth = auth_->ReadLock();
    (void)locked_auth->UpToDate(auth_epoch_);
  }
  return user_or_role_.has_value();
}

bool SafeAuth::HasWebsocketPermission() const {
  auto locked_auth = auth_->ReadLock();
  if (!user_or_role_) {
    return false;
  }
  if (!locked_auth->UpToDate(auth_epoch_)) {
    if (!RefreshCachedPrincipal(*locked_auth, user_or_role_)) {
      return false;
    }
  }
  return PrincipalHasWebsocketPermission(*user_or_role_);
}

bool SafeAuth::AccessControlled() const { return auth_->ReadLock()->AccessControlled(); }

std::unique_ptr<communication::websocket::AuthenticationInterface::SessionAuth> SafeAuth::CreateSessionAuth() const {
  return std::make_unique<PerSessionSafeAuth>(auth_);
}

}  // namespace memgraph::glue
