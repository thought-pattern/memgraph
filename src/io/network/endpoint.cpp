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

#include "io/network/endpoint.hpp"

#include <arpa/inet.h>
#include <fmt/format.h>
#include <netdb.h>
#include <netinet/in.h>
#include <spdlog/spdlog.h>
#include <sys/socket.h>
#include <array>
#include <cerrno>
#include <charconv>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "io/network/network_error.hpp"
#include "utils/message.hpp"
#include "utils/on_scope_exit.hpp"
#include "utils/string.hpp"

namespace {
constexpr std::string_view delimiter = ":";

std::string NormalizeHostAddress(std::string_view raw) {
  if (raw.size() >= 2 && raw.front() == '[' && raw.back() == ']') {
    return std::string{raw.substr(1, raw.size() - 2)};
  }
  return std::string{raw};
}

std::string FormatHostPort(std::string_view host, uint16_t port) {
  if (host.contains(':')) {
    return fmt::format("[{}]:{}", host, port);
  }
  return fmt::format("{}{}{}", host, delimiter, port);
}

bool IsValidBareIpv6Literal(std::string_view candidate) {
  if (candidate.empty() || !candidate.contains(':')) {
    return false;
  }
  const auto zone_sep = candidate.find('%');
  const std::string addr_only{zone_sep == std::string_view::npos ? candidate : candidate.substr(0, zone_sep)};
  if (zone_sep != std::string_view::npos && zone_sep + 1 >= candidate.size()) {
    return false;
  }
  in6_addr parsed_v6{};
  return inet_pton(AF_INET6, addr_only.c_str(), &parsed_v6) == 1;
}
}  // namespace

namespace memgraph::io::network {

Endpoint::Endpoint(std::string address, uint16_t port)
    : address_(NormalizeHostAddress(address)), port_(port) {}

auto Endpoint::SocketAddress() const -> std::string { return FormatHostPort(address_, port_); }

auto Endpoint::GetResolvedSocketAddress() const -> std::string {
  auto const result = TryResolveAddress(address_, port_);
  if (!result) {
    throw NetworkError("Couldn't resolve {}.", FormatHostPort(address_, port_));
  }

  auto resolved_address = std::get<0>(*result);
  auto resolved_port = std::get<1>(*result);

  return FormatHostPort(resolved_address, resolved_port);
}

auto Endpoint::GetResolvedIPAddress() const -> std::string {
  auto const result = TryResolveAddress(address_, port_);
  if (!result) {
    throw NetworkError("Couldn't resolve {}:{}.", address_, port_);
  }

  auto resolved_address = std::get<0>(*result);

  return resolved_address;
}

[[nodiscard]] auto Endpoint::GetIpFamily() const -> IpFamily {
  auto const result = TryResolveAddress(address_, port_);
  if (!result) {
    throw NetworkError("Couldn't resolve {}:{}.", address_, port_);
  }

  return std::get<2>(*result);
}

std::optional<Endpoint::RetValue> Endpoint::TryResolveAddress(std::string_view address, uint16_t port) {
  auto const process_ipv4_family = [address](addrinfo *socket_addr, uint16_t port) -> std::optional<RetValue> {
    std::array<char, INET_ADDRSTRLEN> buffer;  // intentionally uninitialized
    auto *socket_address_ipv4 = reinterpret_cast<struct sockaddr_in *>(socket_addr->ai_addr);
    auto const *res = inet_ntop(socket_addr->ai_family, &(socket_address_ipv4->sin_addr), buffer.data(), buffer.size());
    if (res == NULL) {    // NOLINT
      int errsv = errno;  // don't reorder, otherwise errno could get reassigned.
      spdlog::error("inet_ntop failed with errno {} when resolving {} to ipv4 address.", errsv, address);
      return std::nullopt;
    }
    return std::tuple{std::string{buffer.data()}, port, Endpoint::IpFamily::IP4};
  };

  auto const process_ipv6_family = [address](addrinfo *socket_addr, uint16_t port) -> std::optional<RetValue> {
    std::array<char, INET6_ADDRSTRLEN> buffer;  // intentionally uninitialized
    auto *socket_address_ipv6 = reinterpret_cast<sockaddr_in6 *>(socket_addr->ai_addr);
    auto const *res =
        inet_ntop(socket_addr->ai_family, &(socket_address_ipv6->sin6_addr), buffer.data(), buffer.size());
    if (res == NULL) {    // NOLINT
      int errsv = errno;  // don't reorder, otherwise errno could get reassigned.
      spdlog::error("inet_ntop failed with errno {} when resolving {} to ipv6 address.", errsv, address);
      return std::nullopt;
    }
    return std::tuple{std::string{buffer.data()}, port, Endpoint::IpFamily::IP6};
  };

  auto const parse_ip_family = [&address, &port](
                                   std::function<std::optional<RetValue>(addrinfo *, uint16_t)> const &processing_fn,
                                   auto family) -> std::optional<RetValue> {
    addrinfo const hints{
        .ai_flags = AI_PASSIVE,      // fill with IPv4 or IPv6
        .ai_family = family,         // IPv4 or IPv6
        .ai_socktype = SOCK_STREAM,  // TCP socket
        .ai_protocol = 0,            // any protocol
    };

    addrinfo *info{nullptr};
    utils::OnScopeExit const free_info{[&info]() {
      if (info) {
        freeaddrinfo(info);
      }
    }};

    auto status = getaddrinfo(std::string(address).c_str(), std::to_string(port).c_str(), &hints, &info);
    if (status != 0) {
      spdlog::error("getaddrinfo finished unsuccessfully while resolving {}:{}. Error occurred: {}",
                    address,
                    port,
                    gai_strerror(status));
      return std::nullopt;
    }

    auto *socket_addr = info;
    while (socket_addr != nullptr) {
      if (family != socket_addr->ai_family) {
        socket_addr = socket_addr->ai_next;
        continue;
      }
      return processing_fn(socket_addr, port);
    }

    return std::nullopt;
  };

  auto ip_v4_family = parse_ip_family(process_ipv4_family, AF_INET);
  if (ip_v4_family) {
    return std::move(*ip_v4_family);
  }
  return parse_ip_family(process_ipv6_family, AF_INET6);
}

std::optional<Endpoint> Endpoint::ParseAndCreateSocketOrAddress(std::string_view address,
                                                                std::optional<uint16_t> default_port) {
  if (address.empty()) {
    return std::nullopt;
  }

  if (address.front() == '[') {
    const auto close_bracket = address.find(']');
    if (close_bracket == std::string_view::npos || close_bracket <= 1) {
      return std::nullopt;
    }
    const auto host_part = address.substr(1, close_bracket - 1);
    if (!IsValidBareIpv6Literal(host_part)) {
      return std::nullopt;
    }
    const auto remainder = address.substr(close_bracket + 1);
    if (remainder.empty()) {
      if (!default_port) {
        return std::nullopt;
      }
      return Endpoint{std::string{host_part}, *default_port};
    }
    if (!remainder.starts_with(delimiter)) {
      return std::nullopt;
    }
    const auto parsed_port = ParseValidatedPort(remainder.substr(delimiter.size()));
    if (!parsed_port) {
      return std::nullopt;
    }
    return Endpoint{std::string{host_part}, *parsed_port};
  }

  const auto first_colon = address.find(':');
  if (first_colon == std::string_view::npos) {
    if (!default_port) {
      return std::nullopt;
    }
    return Endpoint{std::string{address}, *default_port};
  }

  const auto last_colon = address.rfind(':');
  if (first_colon != last_colon) {
    // Multiple colons without brackets: valid only as a bare IPv6 literal when default_port is supplied.
    if (default_port && IsValidBareIpv6Literal(address)) {
      return Endpoint{std::string{address}, *default_port};
    }
    return std::nullopt;
  }

  const auto host_part = address.substr(0, first_colon);
  const auto port_part = address.substr(first_colon + 1);
  if (host_part.empty()) {
    return std::nullopt;
  }
  const auto parsed_port = ParseValidatedPort(port_part);
  if (!parsed_port) {
    return std::nullopt;
  }
  return Endpoint{std::string{host_part}, *parsed_port};
}

auto Endpoint::ParseValidatedPort(std::string_view port_str) -> std::optional<uint16_t> {
  if (port_str.empty() || port_str.front() == '+' || port_str.front() == '-') {
    spdlog::error(
        utils::MessageWithLink("Invalid port number '{}'. The port number must be an integer in [0, 65535].",
                               port_str,
                               "https://memgr.ph/ports"));
    return std::nullopt;
  }

  int64_t parsed = -1;
  const auto *begin = port_str.data();
  const auto *end = port_str.data() + port_str.size();
  const auto [ptr, ec] = std::from_chars(begin, end, parsed);
  if (ec != std::errc{} || ptr != end || parsed < 0) {
    spdlog::error(
        utils::MessageWithLink("Invalid port number '{}'. The port number must be a non-negative integer.",
                               port_str,
                               "https://memgr.ph/ports"));
    return std::nullopt;
  }
  if (parsed > static_cast<int64_t>(std::numeric_limits<uint16_t>::max())) {
    spdlog::error(utils::MessageWithLink("Invalid port number. The port number exceedes the maximum possible size.",
                                         "https://memgr.ph/ports"));
    return std::nullopt;
  }

  return static_cast<uint16_t>(parsed);
}

void Endpoint::SetAddress(std::string address) { address_ = NormalizeHostAddress(address); }

void Endpoint::SetPort(uint16_t port) { port_ = port; }

void to_json(nlohmann::json &j, Endpoint const &config) {
  j = nlohmann::json{{"address", config.GetAddress()}, {"port", config.GetPort()}};
}

void from_json(nlohmann::json const &j, Endpoint &config) {
  config.SetAddress(j.at("address").get<std::string>());
  config.SetPort(j.at("port").get<uint16_t>());
}

}  // namespace memgraph::io::network
