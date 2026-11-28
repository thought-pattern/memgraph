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

#include "communication/websocket/listener.hpp"

#include <exception>

#include "communication/fmt.hpp"

namespace memgraph::communication::websocket {
namespace {
void LogError(boost::beast::error_code ec, const std::string_view what) {
  spdlog::warn("Websocket listener failed on {}: {}", what, ec.message());
}
}  // namespace

void Listener::Run() { DoAccept(); }

void Listener::WriteToAll(std::shared_ptr<std::string> message) {
  auto sessions_ptr = sessions_.Lock();
  for (auto &session : *sessions_ptr) {
    session->Write(message);
  }
}

std::optional<boost::asio::ip::tcp::endpoint> Listener::GetEndpoint() const {
  try {
    return acceptor_.local_endpoint();
  } catch (const boost::system::system_error &) {
    return std::nullopt;
  }
}

Listener::Listener(boost::asio::io_context &ioc, ServerContext *context, tcp::endpoint endpoint,
                   AuthenticationInterface &auth)
    : ioc_(ioc), context_(context), acceptor_(ioc), retry_timer_(ioc), auth_(auth) {
  boost::beast::error_code ec;

  // Open the acceptor
  acceptor_.open(endpoint.protocol(), ec);
  if (ec) {
    LogError(ec, "open");
    error_happened_.store(true, std::memory_order_release);
    return;
  }

  // Allow address reuse
  acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
  if (ec) {
    LogError(ec, "set_option");
    error_happened_.store(true, std::memory_order_release);
    return;
  }

  // Bind to the server address
  acceptor_.bind(endpoint, ec);
  if (ec) {
    LogError(ec, "bind");
    error_happened_.store(true, std::memory_order_release);
    return;
  }

  acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
  if (ec) {
    LogError(ec, "listen");
    error_happened_.store(true, std::memory_order_release);
    return;
  }

  spdlog::info("WebSocket server is listening on {}", endpoint);
}

void Listener::DoAccept() {
  acceptor_.async_accept(
      ioc_, [shared_this = shared_from_this()](auto ec, auto socket) { shared_this->OnAccept(ec, std::move(socket)); });
}

void Listener::RegisterSession(std::shared_ptr<Session> session) {
  auto sessions_ptr = sessions_.Lock();
  std::erase_if(*sessions_ptr, [](const auto &elem) { return !elem->IsConnected(); });
  sessions_ptr->emplace_back(std::move(session));
}

void Listener::OnAccept(boost::beast::error_code ec, tcp::socket socket) {
  if (ec) {
    if (IsTransientAcceptError(ec)) {
      accept_log_throttle_.Warn("Websocket listener", ec.message());
      RetryAcceptLater(retry_timer_, [shared_this = shared_from_this()] { shared_this->DoAccept(); });
      return;
    }
    if (ec != boost::asio::error::operation_aborted) {
      error_happened_.store(true, std::memory_order_release);
      LogError(ec, "accept");
    }
    return;
  }

  try {
    auto session = Session::Create(std::move(socket), *context_, auth_);
    session->Run([weak_self = weak_from_this()](std::shared_ptr<Session> ready) {
      if (auto self = weak_self.lock()) {
        self->RegisterSession(std::move(ready));
      }
    });
  } catch (const std::exception &e) {
    spdlog::warn("Websocket listener failed to initialize accepted session: {}", e.what());
  }

  if (acceptor_.is_open()) {
    DoAccept();
  }
}
}  // namespace memgraph::communication::websocket
