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

#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>

#include "utils/string.hpp"

namespace memgraph::utils {

inline constexpr std::string_view kRedactedMask = "****";
inline constexpr std::string_view kRedactedLiteral = "'****'";

inline bool LooksLikePasswordHash(std::string_view text) {
  const auto trimmed = Trim(text);
  return StartsWith(ToLowerCase(trimmed), "bcrypt:$2") || StartsWith(ToLowerCase(trimmed), "pbkdf2-sha256:");
}

inline bool IsSensitiveParamKey(std::string_view key) {
  std::string normalized;
  normalized.reserve(key.size());
  for (unsigned char ch : key) {
    if (std::isalnum(ch)) {
      normalized.push_back(static_cast<char>(std::tolower(ch)));
    }
  }
  return normalized == "password" || normalized == "oldpassword" || normalized == "newpassword" ||
         normalized == "currentpassword" || normalized == "userpassword" || normalized == "passwd" ||
         normalized == "passphrase" || normalized == "pwd" || normalized == "secret" ||
         normalized == "clientsecret" || normalized == "apikey" || normalized == "accesstoken" ||
         normalized == "refreshtoken" || normalized == "idtoken" || normalized == "authtoken" ||
         normalized == "bearertoken" || normalized == "credentials";
}

namespace detail {

inline size_t ScanWhitespaceOrComment(std::string_view s, size_t pos) {
  if (pos >= s.size()) return 0;
  if (std::isspace(static_cast<unsigned char>(s[pos]))) {
    size_t end = pos + 1;
    while (end < s.size() && std::isspace(static_cast<unsigned char>(s[end]))) {
      ++end;
    }
    return end - pos;
  }
  if (pos + 1 < s.size() && s[pos] == '/' && s[pos + 1] == '/') {
    size_t end = pos + 2;
    while (end < s.size() && s[end] != '\n' && s[end] != '\r') {
      ++end;
    }
    return end - pos;
  }
  if (pos + 1 < s.size() && s[pos] == '/' && s[pos + 1] == '*') {
    size_t end = pos + 2;
    while (end + 1 < s.size() && !(s[end] == '*' && s[end + 1] == '/')) {
      ++end;
    }
    return (end + 1 < s.size()) ? (end + 2 - pos) : (s.size() - pos);
  }
  return 0;
}

inline size_t ScanQuotedLiteral(std::string_view s, size_t pos, std::string *decoded) {
  const char quote = s[pos];
  size_t i = pos + 1;
  if (decoded) decoded->clear();
  while (i < s.size()) {
    if (s[i] == '\\') {
      if (i + 1 < s.size()) {
        if (decoded) decoded->push_back(s[i + 1]);
        i += 2;
      } else {
        ++i;
      }
      continue;
    }
    if (s[i] == quote) {
      if (i + 1 < s.size() && s[i + 1] == quote) {
        if (decoded) decoded->push_back(quote);
        i += 2;
        continue;
      }
      return i + 1 - pos;
    }
    if (decoded) decoded->push_back(s[i]);
    ++i;
  }
  return s.size() - pos;
}

inline size_t ScanBacktickIdentifier(std::string_view s, size_t pos, std::string *unescaped) {
  size_t i = pos + 1;
  if (unescaped) unescaped->clear();
  while (i < s.size()) {
    if (s[i] == '`') {
      if (i + 1 < s.size() && s[i + 1] == '`') {
        if (unescaped) unescaped->push_back('`');
        i += 2;
        continue;
      }
      return i + 1 - pos;
    }
    if (unescaped) unescaped->push_back(s[i]);
    ++i;
  }
  return s.size() - pos;
}

}  // namespace detail

inline std::string RedactQueryCredentials(std::string_view query,
                                          std::unordered_set<std::string> *redacted_param_names = nullptr) {
  std::string out;
  out.reserve(query.size());

  std::string prev_word;
  bool in_password_stmt = false;
  bool expect_secret = false;

  for (size_t i = 0; i < query.size();) {
    if (const size_t trivia_len = detail::ScanWhitespaceOrComment(query, i); trivia_len > 0) {
      out.append(query.substr(i, trivia_len));
      i += trivia_len;
      continue;
    }

    const char ch = query[i];
    if (ch == '\'' || ch == '"') {
      std::string decoded;
      const size_t len = detail::ScanQuotedLiteral(query, i, &decoded);
      const bool direct_set_password = in_password_stmt && prev_word == "password";
      if (expect_secret || direct_set_password || LooksLikePasswordHash(decoded)) {
        out.append(kRedactedLiteral);
        expect_secret = false;
      } else {
        out.append(query.substr(i, len));
      }
      prev_word.clear();
      i += len;
      continue;
    }

    if (ch == '`') {
      const size_t len = detail::ScanBacktickIdentifier(query, i, nullptr);
      if (expect_secret) {
        out.append(kRedactedLiteral);
        expect_secret = false;
      } else {
        out.append(query.substr(i, len));
      }
      prev_word.clear();
      i += len;
      continue;
    }

    if (ch == '$') {
      size_t cursor = i + 1;
      while (cursor < query.size()) {
        const size_t ws = detail::ScanWhitespaceOrComment(query, cursor);
        if (ws == 0) break;
        cursor += ws;
      }
      std::string param_name;
      if (cursor < query.size() && query[cursor] == '`') {
        const size_t bt_len = detail::ScanBacktickIdentifier(query, cursor, &param_name);
        cursor += bt_len;
      } else {
        const size_t name_start = cursor;
        while (cursor < query.size() &&
               (std::isalnum(static_cast<unsigned char>(query[cursor])) || query[cursor] == '_')) {
          ++cursor;
        }
        param_name.assign(query.substr(name_start, cursor - name_start));
      }
      const size_t len = cursor > i + 1 ? (cursor - i) : 1;
      const bool direct_set_password = in_password_stmt && prev_word == "password";
      if ((expect_secret || direct_set_password) && !param_name.empty()) {
        if (redacted_param_names) {
          redacted_param_names->insert(param_name);
        }
        expect_secret = false;
      }
      out.append(query.substr(i, len));
      prev_word.clear();
      i += len;
      continue;
    }

    if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
      size_t end = i + 1;
      while (end < query.size() &&
             (std::isalnum(static_cast<unsigned char>(query[end])) || query[end] == '_')) {
        ++end;
      }
      const std::string_view raw_word = query.substr(i, end - i);
      const std::string lower_word = ToLowerCase(raw_word);

      if (expect_secret) {
        if (lower_word == "null") {
          out.append(raw_word);
        } else {
          out.append(kRedactedLiteral);
        }
        expect_secret = false;
        prev_word = lower_word;
        i = end;
        continue;
      }

      if (prev_word == "identified" && lower_word == "by") {
        expect_secret = true;
      } else if (prev_word == "set" && lower_word == "password") {
        in_password_stmt = true;
      } else if (in_password_stmt && (lower_word == "to" || lower_word == "replace" || lower_word == "from")) {
        expect_secret = true;
      }

      out.append(raw_word);
      prev_word = lower_word;
      i = end;
      continue;
    }

    if (std::isdigit(static_cast<unsigned char>(ch)) ||
        ((ch == '-' || ch == '+') && i + 1 < query.size() && std::isdigit(static_cast<unsigned char>(query[i + 1])))) {
      size_t end = i + 1;
      while (end < query.size() &&
             (std::isalnum(static_cast<unsigned char>(query[end])) || query[end] == '.' || query[end] == '_')) {
        ++end;
      }
      if (expect_secret) {
        out.append(kRedactedLiteral);
        expect_secret = false;
      } else {
        out.append(query.substr(i, end - i));
      }
      prev_word.clear();
      i = end;
      continue;
    }

    if (ch == ';') {
      in_password_stmt = false;
      expect_secret = false;
    } else if (expect_secret) {
      out.append(kRedactedLiteral);
      expect_secret = false;
      prev_word.clear();
      ++i;
      continue;
    }
    out.push_back(ch);
    prev_word.clear();
    ++i;
  }

  return out;
}

inline std::string EncodeAuditCsvField(std::string_view src) {
  bool needs_quotes = false;
  for (char c : src) {
    if (c == ',' || c == '"' || c == '\\' || c == '\n' || c == '\r') {
      needs_quotes = true;
      break;
    }
  }
  if (!needs_quotes) {
    return std::string(src);
  }
  std::string out;
  out.reserve(src.size() + 2);
  out.push_back('"');
  for (char c : src) {
    if (c == '\\' || c == '"') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

}  // namespace memgraph::utils
