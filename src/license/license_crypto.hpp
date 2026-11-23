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

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace memgraph::license::crypto {

inline constexpr size_t kSha256Bytes = 32;
inline constexpr size_t kRsa2048Bytes = 256;
inline constexpr size_t kRsa2048Limbs = kRsa2048Bytes / sizeof(uint64_t);

using Rsa2048Modulus = std::array<uint8_t, kRsa2048Bytes>;
using Rsa2048Signature = std::array<uint8_t, kRsa2048Bytes>;
using Rsa2048Exponent = std::array<uint8_t, kRsa2048Bytes>;

// Trusted Memgraph license-issuer 2048-bit RSA public modulus (big-endian, public exponent e = 65537).
inline constexpr Rsa2048Modulus kDefaultIssuerRsaModulus = {
    0x86, 0xf4, 0xe0, 0x87, 0x52, 0xad, 0x57, 0x8b, 0xce, 0xf0, 0xd0, 0x27, 0xe7, 0x81, 0xad, 0x1c,
    0x44, 0x43, 0x9b, 0x02, 0xa2, 0xdb, 0x66, 0x25, 0x95, 0xa9, 0x49, 0x94, 0x01, 0xd4, 0x85, 0x55,
    0x24, 0x84, 0xe6, 0x9f, 0x9d, 0x4b, 0x0b, 0x06, 0xaf, 0xe8, 0xa4, 0xf7, 0x3d, 0x5d, 0xf4, 0x18,
    0x12, 0x38, 0x02, 0x12, 0x09, 0xcb, 0x8f, 0x81, 0x3c, 0x48, 0x0d, 0xcc, 0xe6, 0x7c, 0x7c, 0xde,
    0x2f, 0x40, 0xa7, 0x47, 0xd6, 0x4d, 0x0d, 0xff, 0x03, 0x6c, 0x1d, 0x79, 0x16, 0x8f, 0xe5, 0xca,
    0x95, 0x58, 0x0a, 0x5e, 0xdd, 0x66, 0xfc, 0x13, 0xc4, 0x97, 0x57, 0x11, 0x42, 0x44, 0x99, 0xc2,
    0x92, 0xa9, 0x4f, 0x50, 0xf9, 0x8c, 0x4c, 0x89, 0x85, 0x2a, 0x50, 0x89, 0xb7, 0x34, 0xcf, 0x87,
    0xea, 0xf1, 0xc4, 0x2f, 0x6b, 0x87, 0x8b, 0xd1, 0xb0, 0x5e, 0x9f, 0xe8, 0xd2, 0x78, 0x6e, 0x49,
    0x22, 0x29, 0x14, 0xa2, 0x3c, 0x4b, 0xf8, 0x0e, 0xe8, 0x26, 0x12, 0x46, 0x15, 0x1c, 0x7b, 0x7e,
    0xde, 0x3f, 0x83, 0x80, 0xd3, 0x01, 0xd3, 0x17, 0xf5, 0xb8, 0xd8, 0x3d, 0xb8, 0xec, 0x09, 0x3f,
    0x4e, 0xec, 0x4c, 0xa2, 0x73, 0x11, 0xca, 0x72, 0x6d, 0xef, 0xc8, 0x52, 0x44, 0x20, 0xbb, 0xc1,
    0x35, 0x15, 0x5a, 0x98, 0x07, 0x1b, 0xa3, 0xe8, 0x94, 0x38, 0xd2, 0xae, 0x12, 0x72, 0x9c, 0x8f,
    0xcc, 0x98, 0x32, 0x56, 0x8f, 0x1a, 0x0e, 0x02, 0xcc, 0x9d, 0xe0, 0xe8, 0xa2, 0x45, 0x30, 0x7e,
    0x67, 0x4e, 0x82, 0x2a, 0xe3, 0x11, 0x63, 0xcc, 0x6a, 0x58, 0xa9, 0xdd, 0xdd, 0x60, 0xad, 0xe2,
    0xde, 0x49, 0xfc, 0x60, 0x8c, 0x63, 0x4d, 0x54, 0xb0, 0x22, 0xb9, 0xad, 0xf9, 0x47, 0x0a, 0x01,
    0x29, 0x95, 0x40, 0x09, 0x27, 0x1f, 0x4a, 0xc8, 0x1d, 0xed, 0x37, 0x80, 0xc3, 0x2e, 0x80, 0x71};

namespace detail {

constexpr uint32_t Rotr32(uint32_t x, uint32_t n) noexcept { return (x >> n) | (x << (32U - n)); }

inline void Sha256Transform(std::array<uint32_t, 8> &state, const uint8_t block[64]) noexcept {
  static constexpr std::array<uint32_t, 64> kRoundConstants = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
      0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
      0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
      0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
      0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
      0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
      0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  std::array<uint32_t, 64> w{};
  for (size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24U) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16U) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8U) | static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 = Rotr32(w[i - 15], 7U) ^ Rotr32(w[i - 15], 18U) ^ (w[i - 15] >> 3U);
    const uint32_t s1 = Rotr32(w[i - 2], 17U) ^ Rotr32(w[i - 2], 19U) ^ (w[i - 2] >> 10U);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];
  uint32_t e = state[4];
  uint32_t f = state[5];
  uint32_t g = state[6];
  uint32_t h = state[7];

  for (size_t i = 0; i < 64; ++i) {
    const uint32_t s1 = Rotr32(e, 6U) ^ Rotr32(e, 11U) ^ Rotr32(e, 25U);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t temp1 = h + s1 + ch + kRoundConstants[i] + w[i];
    const uint32_t s0 = Rotr32(a, 2U) ^ Rotr32(a, 13U) ^ Rotr32(a, 22U);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

using Limbs2048 = std::array<uint64_t, kRsa2048Limbs>;

inline Limbs2048 FromBigEndianBytes(std::span<const uint8_t, kRsa2048Bytes> bytes) noexcept {
  Limbs2048 limbs{};
  for (size_t i = 0; i < kRsa2048Limbs; ++i) {
    const size_t base = (kRsa2048Limbs - 1 - i) * 8;
    uint64_t val = 0;
    for (size_t b = 0; b < 8; ++b) {
      val = (val << 8U) | static_cast<uint64_t>(bytes[base + b]);
    }
    limbs[i] = val;
  }
  return limbs;
}

inline Rsa2048Signature ToBigEndianBytes(const Limbs2048 &limbs) noexcept {
  Rsa2048Signature out{};
  for (size_t i = 0; i < kRsa2048Limbs; ++i) {
    const size_t base = (kRsa2048Limbs - 1 - i) * 8;
    uint64_t val = limbs[i];
    for (size_t b = 0; b < 8; ++b) {
      out[base + 7 - b] = static_cast<uint8_t>(val & 0xFFU);
      val >>= 8U;
    }
  }
  return out;
}

inline bool LessThan(const Limbs2048 &a, const Limbs2048 &b) noexcept {
  for (size_t i = kRsa2048Limbs; i > 0; --i) {
    if (a[i - 1] != b[i - 1]) {
      return a[i - 1] < b[i - 1];
    }
  }
  return false;
}

inline void SubInPlace(Limbs2048 &a, const Limbs2048 &b) noexcept {
  uint64_t borrow = 0;
  for (size_t i = 0; i < kRsa2048Limbs; ++i) {
    const unsigned __int128 sub = static_cast<unsigned __int128>(b[i]) + borrow;
    const uint64_t bi = static_cast<uint64_t>(sub);
    const uint64_t next_borrow = (sub >> 64U) != 0 || a[i] < bi ? 1U : 0U;
    a[i] = a[i] - bi;
    borrow = next_borrow;
  }
}

inline void DoubleModInPlace(Limbs2048 &a, const Limbs2048 &mod) noexcept {
  uint64_t carry = 0;
  for (size_t i = 0; i < kRsa2048Limbs; ++i) {
    const uint64_t next_carry = a[i] >> 63U;
    a[i] = (a[i] << 1U) | carry;
    carry = next_carry;
  }
  if (carry != 0 || !LessThan(a, mod)) {
    SubInPlace(a, mod);
  }
}

inline uint64_t ComputeNegModInv64(uint64_t n0) noexcept {
  uint64_t inv = 1;
  for (int i = 0; i < 6; ++i) {
    inv *= 2ULL - n0 * inv;
  }
  return 0ULL - inv;
}

inline Limbs2048 ComputeR2ModN(const Limbs2048 &mod) noexcept {
  Limbs2048 r{};
  r[0] = 1;
  // R = 2^2048, so R^2 = 2^4096 mod N via 4096 modular doublings.
  for (size_t bit = 0; bit < kRsa2048Bytes * 16; ++bit) {
    DoubleModInPlace(r, mod);
  }
  return r;
}

inline Limbs2048 MontMul(const Limbs2048 &a, const Limbs2048 &b, const Limbs2048 &mod, uint64_t n0_prime) noexcept {
  std::array<uint64_t, kRsa2048Limbs + 2> t{};
  for (size_t i = 0; i < kRsa2048Limbs; ++i) {
    uint64_t carry = 0;
    for (size_t j = 0; j < kRsa2048Limbs; ++j) {
      const unsigned __int128 prod =
          static_cast<unsigned __int128>(a[j]) * b[i] + t[j] + carry;
      t[j] = static_cast<uint64_t>(prod);
      carry = static_cast<uint64_t>(prod >> 64U);
    }
    const unsigned __int128 top_sum = static_cast<unsigned __int128>(t[kRsa2048Limbs]) + carry;
    t[kRsa2048Limbs] = static_cast<uint64_t>(top_sum);
    t[kRsa2048Limbs + 1] = static_cast<uint64_t>(top_sum >> 64U);

    const uint64_t m = t[0] * n0_prime;
    unsigned __int128 red = static_cast<unsigned __int128>(m) * mod[0] + t[0];
    uint64_t red_carry = static_cast<uint64_t>(red >> 64U);
    for (size_t j = 1; j < kRsa2048Limbs; ++j) {
      red = static_cast<unsigned __int128>(m) * mod[j] + t[j] + red_carry;
      t[j - 1] = static_cast<uint64_t>(red);
      red_carry = static_cast<uint64_t>(red >> 64U);
    }
    const unsigned __int128 final_sum = static_cast<unsigned __int128>(t[kRsa2048Limbs]) + red_carry;
    t[kRsa2048Limbs - 1] = static_cast<uint64_t>(final_sum);
    t[kRsa2048Limbs] = t[kRsa2048Limbs + 1] + static_cast<uint64_t>(final_sum >> 64U);
    t[kRsa2048Limbs + 1] = 0;
  }

  Limbs2048 out{};
  for (size_t i = 0; i < kRsa2048Limbs; ++i) {
    out[i] = t[i];
  }
  if (t[kRsa2048Limbs] != 0 || !LessThan(out, mod)) {
    SubInPlace(out, mod);
  }
  return out;
}

inline std::array<uint8_t, kRsa2048Bytes> BuildPkcs1V15Sha256Block(
    const std::array<uint8_t, kSha256Bytes> &digest) noexcept {
  // DER DigestInfo prefix for SHA-256 (RFC 8017 Section 9.2 Notes 1).
  static constexpr std::array<uint8_t, 19> kSha256DigestInfoPrefix = {
      0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
      0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
  constexpr size_t kPsLen = kRsa2048Bytes - 3 - kSha256DigestInfoPrefix.size() - kSha256Bytes;

  std::array<uint8_t, kRsa2048Bytes> em{};
  em[0] = 0x00;
  em[1] = 0x01;
  for (size_t i = 0; i < kPsLen; ++i) {
    em[2 + i] = 0xFF;
  }
  em[2 + kPsLen] = 0x00;
  std::memcpy(em.data() + 3 + kPsLen, kSha256DigestInfoPrefix.data(), kSha256DigestInfoPrefix.size());
  std::memcpy(em.data() + 3 + kPsLen + kSha256DigestInfoPrefix.size(), digest.data(), digest.size());
  return em;
}

}  // namespace detail

inline std::array<uint8_t, kSha256Bytes> Sha256(std::span<const uint8_t> message) noexcept {
  std::array<uint32_t, 8> state = {
      0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

  size_t offset = 0;
  while (offset + 64 <= message.size()) {
    detail::Sha256Transform(state, message.data() + offset);
    offset += 64;
  }

  std::array<uint8_t, 128> tail{};
  const size_t rem = message.size() - offset;
  if (rem > 0) {
    std::memcpy(tail.data(), message.data() + offset, rem);
  }
  tail[rem] = 0x80U;

  const size_t padded_bytes = (rem < 56) ? 64 : 128;
  const uint64_t bit_len = static_cast<uint64_t>(message.size()) * 8ULL;
  for (size_t i = 0; i < 8; ++i) {
    tail[padded_bytes - 1 - i] = static_cast<uint8_t>((bit_len >> (i * 8U)) & 0xFFU);
  }

  detail::Sha256Transform(state, tail.data());
  if (padded_bytes == 128) {
    detail::Sha256Transform(state, tail.data() + 64);
  }

  std::array<uint8_t, kSha256Bytes> digest{};
  for (size_t i = 0; i < 8; ++i) {
    digest[i * 4] = static_cast<uint8_t>((state[i] >> 24U) & 0xFFU);
    digest[i * 4 + 1] = static_cast<uint8_t>((state[i] >> 16U) & 0xFFU);
    digest[i * 4 + 2] = static_cast<uint8_t>((state[i] >> 8U) & 0xFFU);
    digest[i * 4 + 3] = static_cast<uint8_t>(state[i] & 0xFFU);
  }
  return digest;
}

inline std::string Sha256Hex(std::string_view input) {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  const auto bytes = std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(input.data()), input.size());
  const auto digest = Sha256(bytes);
  std::string out;
  out.resize(digest.size() * 2);
  for (size_t i = 0; i < digest.size(); ++i) {
    out[i * 2] = kHexDigits[(digest[i] >> 4U) & 0x0FU];
    out[i * 2 + 1] = kHexDigits[digest[i] & 0x0FU];
  }
  return out;
}

inline bool VerifyRsa2048Sha256Signature(std::span<const uint8_t> message,
                                         std::span<const uint8_t, kRsa2048Bytes> signature,
                                         const Rsa2048Modulus &modulus) noexcept {
  // Require a full 2048-bit odd RSA modulus.
  if ((modulus[0] & 0x80U) == 0U || (modulus[kRsa2048Bytes - 1] & 1U) == 0U) {
    return false;
  }

  const auto mod_limbs = detail::FromBigEndianBytes(modulus);
  const auto sig_limbs = detail::FromBigEndianBytes(signature);
  if (!detail::LessThan(sig_limbs, mod_limbs)) {
    return false;
  }

  const uint64_t n0_prime = detail::ComputeNegModInv64(mod_limbs[0]);
  const auto r2 = detail::ComputeR2ModN(mod_limbs);
  const auto sig_mont = detail::MontMul(sig_limbs, r2, mod_limbs, n0_prime);

  // Public exponent e = 65537 = 2^16 + 1: 16 squarings and 1 multiply.
  auto acc = sig_mont;
  for (int i = 0; i < 16; ++i) {
    acc = detail::MontMul(acc, acc, mod_limbs, n0_prime);
  }
  acc = detail::MontMul(acc, sig_mont, mod_limbs, n0_prime);

  detail::Limbs2048 one{};
  one[0] = 1;
  const auto em_limbs = detail::MontMul(acc, one, mod_limbs, n0_prime);
  const auto em_bytes = detail::ToBigEndianBytes(em_limbs);

  const auto expected_em = detail::BuildPkcs1V15Sha256Block(Sha256(message));
  uint8_t diff = 0;
  for (size_t i = 0; i < kRsa2048Bytes; ++i) {
    diff |= static_cast<uint8_t>(em_bytes[i] ^ expected_em[i]);
  }
  return diff == 0;
}

inline Rsa2048Signature SignRsa2048Sha256(std::span<const uint8_t> message, const Rsa2048Exponent &private_exponent,
                                          const Rsa2048Modulus &modulus) noexcept {
  if ((modulus[0] & 0x80U) == 0U || (modulus[kRsa2048Bytes - 1] & 1U) == 0U) {
    return {};
  }

  const auto mod_limbs = detail::FromBigEndianBytes(modulus);
  const auto em_bytes = detail::BuildPkcs1V15Sha256Block(Sha256(message));
  const auto em_limbs = detail::FromBigEndianBytes(em_bytes);
  if (!detail::LessThan(em_limbs, mod_limbs)) {
    return {};
  }

  const uint64_t n0_prime = detail::ComputeNegModInv64(mod_limbs[0]);
  const auto r2 = detail::ComputeR2ModN(mod_limbs);
  detail::Limbs2048 one{};
  one[0] = 1;
  auto result_mont = detail::MontMul(one, r2, mod_limbs, n0_prime);
  const auto base_mont = detail::MontMul(em_limbs, r2, mod_limbs, n0_prime);

  for (size_t byte_idx = 0; byte_idx < kRsa2048Bytes; ++byte_idx) {
    const uint8_t exp_byte = private_exponent[byte_idx];
    for (int bit = 7; bit >= 0; --bit) {
      result_mont = detail::MontMul(result_mont, result_mont, mod_limbs, n0_prime);
      if (((exp_byte >> static_cast<unsigned>(bit)) & 1U) != 0U) {
        result_mont = detail::MontMul(result_mont, base_mont, mod_limbs, n0_prime);
      }
    }
  }

  const auto sig_limbs = detail::MontMul(result_mont, one, mod_limbs, n0_prime);
  return detail::ToBigEndianBytes(sig_limbs);
}

}  // namespace memgraph::license::crypto
