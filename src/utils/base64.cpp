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

/*
   base64.cpp and base64.h
   base64 encoding and decoding with C++.
   More information at
     https://renenyffenegger.ch/notes/development/Base64/Encoding-and-decoding-base-64-with-cpp
   Version: 2.rc.08 (release candidate)
   Copyright (C) 2004-2017, 2020, 2021 René Nyffenegger
   This source code is provided 'as-is', without any express or implied
   warranty. In no event will the author be held liable for any damages
   arising from the use of this software.
   Permission is granted to anyone to use this software for any purpose,
   including commercial applications, and to alter it and redistribute it
   freely, subject to the following restrictions:
   1. The origin of this source code must not be misrepresented; you must not
      claim that you wrote the original source code. If you use this source code
      in a product, an acknowledgment in the product documentation would be
      appreciated but is not required.
   2. Altered source versions must be plainly marked as such, and must not be
      misrepresented as being the original source code.
   3. This notice may not be removed or altered from any source distribution.
   René Nyffenegger rene.nyffenegger@adp-gmbh.ch
*/

#include "base64.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace memgraph::utils {
namespace {
//
// Depending on the url parameter in base64_chars, one of
// two sets of base64 characters needs to be chosen.
// They differ in their last two characters.
//
inline constexpr std::array base64_chars = {
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"
    "+/",

    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789"
    "-_"};

unsigned int pos_of_char(const unsigned char chr) {
  //
  // Return the position of chr within base64_encode()
  //

  if (chr >= 'A' && chr <= 'Z') return chr - 'A';
  if (chr >= 'a' && chr <= 'z') return chr - 'a' + ('Z' - 'A') + 1;
  if (chr >= '0' && chr <= '9') return chr - '0' + ('Z' - 'A') + ('z' - 'a') + 2;
  if (chr == '+' || chr == '-')
    return 62;  // Be liberal with input and accept both url ('-') and non-url ('+') base 64 characters (
  if (chr == '/' || chr == '_') return 63;  // Ditto for '/' and '_'

  //
  // 2020-10-23: Throw std::exception rather than const char*
  //(Pablo Martin-Gomez, https://github.com/Bouska)
  //
  throw std::runtime_error("Input is not valid base64-encoded data.");
}

std::string insert_linebreaks(std::string str, size_t distance) {
  //
  // Provided by https://github.com/JomaCorpFX, adapted by me.
  //
  if (!str.length()) {
    return "";
  }

  size_t pos = distance;

  while (pos < str.size()) {
    str.insert(pos, "\n");
    pos += distance + 1;
  }

  return str;
}

template <typename String, size_t line_length>
std::string encode_with_line_breaks(String s) {
  return insert_linebreaks(base64_encode(s, false), line_length);
}

template <typename String>
std::string encode_pem(String s) {
  return encode_with_line_breaks<String, 64>(s);
}

template <typename String>
std::string encode_mime(String s) {
  return encode_with_line_breaks<String, 76>(s);
}

template <typename String>
std::string encode(String s, bool url) {
  return base64_encode(reinterpret_cast<const unsigned char *>(s.data()), s.length(), url);
}
}  // namespace

std::string base64_encode(unsigned char const *bytes_to_encode, size_t in_len, bool url) {
  const size_t groups = in_len / 3 + (in_len % 3 != 0 ? 1 : 0);
  if (groups > std::numeric_limits<size_t>::max() / 4) {
    throw std::length_error("Base64 output size overflows size_t.");
  }
  size_t len_encoded = groups * 4;

  unsigned char trailing_char = url ? '.' : '=';

  const char *base64_chars_ = base64_chars[url];

  std::string ret;
  ret.reserve(len_encoded);

  size_t pos = 0;

  while (pos < in_len) {
    // NOLINTNEXTLINE(hicpp-signed-bitwise)
    ret.push_back(base64_chars_[(bytes_to_encode[pos + 0] & 0xfc) >> 2]);

    if (in_len - pos > 1) {
      // NOLINTNEXTLINE(hicpp-signed-bitwise)
      ret.push_back(base64_chars_[((bytes_to_encode[pos + 0] & 0x03) << 4) + ((bytes_to_encode[pos + 1] & 0xf0) >> 4)]);

      if (in_len - pos > 2) {
        ret.push_back(
            // NOLINTNEXTLINE(hicpp-signed-bitwise)
            base64_chars_[((bytes_to_encode[pos + 1] & 0x0f) << 2) + ((bytes_to_encode[pos + 2] & 0xc0) >> 6)]);
        // NOLINTNEXTLINE(hicpp-signed-bitwise)
        ret.push_back(base64_chars_[bytes_to_encode[pos + 2] & 0x3f]);
      } else {
        // NOLINTNEXTLINE(hicpp-signed-bitwise)
        ret.push_back(base64_chars_[(bytes_to_encode[pos + 1] & 0x0f) << 2]);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
        ret.push_back(trailing_char);
      }
    } else {
      // NOLINTNEXTLINE(hicpp-signed-bitwise)
      ret.push_back(base64_chars_[(bytes_to_encode[pos + 0] & 0x03) << 4]);
      // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
      ret.push_back(trailing_char);
      // NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
      ret.push_back(trailing_char);
    }

    if (in_len - pos <= 3) {
      break;
    }
    pos += 3;
  }

  return ret;
}

template <typename String>
static std::string decode(String encoded_string, bool remove_linebreaks) {
  if (encoded_string.empty()) return std::string();

  if (remove_linebreaks) {
    std::string copy(encoded_string);
    std::erase_if(copy, [](char c) { return c == '\n' || c == '\r'; });
    return base64_decode(copy, false);
  }

  size_t length_of_string = encoded_string.length();
  size_t pos = 0;

  size_t approx_length_of_decoded_string = length_of_string / 4 * 3;
  std::string ret;
  ret.reserve(approx_length_of_decoded_string);

  constexpr auto is_padding = [](char c) noexcept { return c == '=' || c == '.'; };

  while (pos < length_of_string) {
    if (length_of_string - pos < 2) {
      throw std::runtime_error("Input is not valid base64-encoded data.");
    }

    size_t pos_of_char_0 = pos_of_char(static_cast<unsigned char>(encoded_string[pos + 0]));
    size_t pos_of_char_1 = pos_of_char(static_cast<unsigned char>(encoded_string[pos + 1]));

    // NOLINTNEXTLINE(hicpp-signed-bitwise)
    ret.push_back(static_cast<std::string::value_type>((pos_of_char_0 << 2) + ((pos_of_char_1 & 0x30) >> 4)));

    if (pos + 2 < length_of_string) {
      if (is_padding(encoded_string[pos + 2])) {
        if (pos + 3 >= length_of_string || !is_padding(encoded_string[pos + 3]) || pos + 4 < length_of_string) {
          throw std::runtime_error("Input is not valid base64-encoded data.");
        }
      } else {
        unsigned int pos_of_char_2 = pos_of_char(static_cast<unsigned char>(encoded_string[pos + 2]));
        ret.push_back(
            // NOLINTNEXTLINE(hicpp-signed-bitwise)
            static_cast<std::string::value_type>(((pos_of_char_1 & 0x0f) << 4) + ((pos_of_char_2 & 0x3c) >> 2)));

        if (pos + 3 < length_of_string) {
          if (is_padding(encoded_string[pos + 3])) {
            if (pos + 4 < length_of_string) {
              throw std::runtime_error("Input is not valid base64-encoded data.");
            }
          } else {
            ret.push_back(
                // NOLINTNEXTLINE(hicpp-signed-bitwise)
                static_cast<std::string::value_type>(((pos_of_char_2 & 0x03) << 6) +
                                                     pos_of_char(static_cast<unsigned char>(encoded_string[pos + 3]))));
          }
        }
      }
    }

    if (length_of_string - pos <= 4) {
      break;
    }
    pos += 4;
  }

  return ret;
}

std::string base64_decode(std::string const &s, bool remove_linebreaks) { return decode(s, remove_linebreaks); }

std::string base64_encode(std::string const &s, bool url) { return encode(s, url); }

std::string base64_encode_pem(std::string const &s) { return encode_pem(s); }

std::string base64_encode_mime(std::string const &s) { return encode_mime(s); }

std::string base64_encode(std::string_view s, bool url) { return encode(s, url); }

std::string base64_encode_pem(std::string_view s) { return encode_pem(s); }

std::string base64_encode_mime(std::string_view s) { return encode_mime(s); }

std::string base64_decode(std::string_view s, bool remove_linebreaks) { return decode(s, remove_linebreaks); }

}  // namespace memgraph::utils
