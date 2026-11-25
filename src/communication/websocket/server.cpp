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

#include "communication/websocket/server.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <string_view>

namespace memgraph::communication::websocket {

namespace {

std::string SanitizeUtf8(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  size_t i = 0;
  while (i < input.size()) {
    const auto b0 = static_cast<uint8_t>(input[i]);
    if (b0 < 0x80U) {
      out.push_back(static_cast<char>(b0));
      ++i;
      continue;
    }
    size_t seq_len = 0;
    uint32_t codepoint = 0;
    uint32_t min_cp = 0;
    if ((b0 & 0xE0U) == 0xC0U) {
      seq_len = 2;
      codepoint = b0 & 0x1FU;
      min_cp = 0x80U;
    } else if ((b0 & 0xF0U) == 0xE0U) {
      seq_len = 3;
      codepoint = b0 & 0x0FU;
      min_cp = 0x800U;
    } else if ((b0 & 0xF8U) == 0xF0U) {
      seq_len = 4;
      codepoint = b0 & 0x07U;
      min_cp = 0x10000U;
    }
    bool valid = (seq_len > 0 && i + seq_len <= input.size());
    for (size_t k = 1; valid && k < seq_len; ++k) {
      const auto bk = static_cast<uint8_t>(input[i + k]);
      if ((bk & 0xC0U) != 0x80U) {
        valid = false;
      } else {
        codepoint = (codepoint << 6U) | (bk & 0x3FU);
      }
    }
    if (valid && codepoint >= min_cp && codepoint <= 0x10FFFFU && (codepoint < 0xD800U || codepoint > 0xDFFFU)) {
      out.append(input.data() + i, seq_len);
      i += seq_len;
    } else {
      out.append("\xEF\xBF\xBD", 3);
      ++i;
    }
  }
  return out;
}

}  // namespace

Server::~Server() {
  MG_ASSERT(!background_thread_ || (ioc_.stopped() && !background_thread_->joinable()),
            "Server wasn't shutdown properly");
}

void Server::Start() {
  MG_ASSERT(!background_thread_, "The server was already started!");
  if (listener_->HasErrorHappened()) {
    spdlog::error("We have error on websocket listener already! Aborting server start.");
    return;
  }
  listener_->Run();
  background_thread_.emplace([this] { ioc_.run(); });
}

void Server::Shutdown() {
  if (ioc_.stopped()) {
    spdlog::trace("Websocket is already stopped!");
    return;
  }
  ioc_.stop();
}

void Server::AwaitShutdown() {
  if (background_thread_ && background_thread_->joinable()) {
    background_thread_->join();
  }
}

bool Server::IsRunning() const { return !listener_->HasErrorHappened() && background_thread_ && !ioc_.stopped(); }

bool Server::HasErrorHappened() const { return listener_->HasErrorHappened(); }

std::optional<boost::asio::ip::tcp::endpoint> Server::GetEndpoint() const { return listener_->GetEndpoint(); }

void Server::LoggingSink::sink_it_(const spdlog::details::log_msg &msg) {
  const auto listener = listener_.lock();
  if (!listener) {
    return;
  }
  const auto level_sv = spdlog::level::to_string_view(msg.level);
  const std::string_view payload_sv{msg.payload.data(), msg.payload.size()};
  nlohmann::json event = {
      {"event", "log"},
      {"level", std::string_view{level_sv.data(), level_sv.size()}},
      {"message", SanitizeUtf8(payload_sv)},
  };
  listener->WriteToAll(std::make_shared<std::string>(event.dump()));
}

std::shared_ptr<Server::LoggingSink> Server::GetLoggingSink() { return std::make_shared<LoggingSink>(listener_); }

}  // namespace memgraph::communication::websocket
