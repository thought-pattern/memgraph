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

#include <atomic>
#include <exception>
#include <list>
#include <memory>

#include <spdlog/spdlog.h>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>

#include "communication/accept_retry.hpp"
#include "communication/context.hpp"
#include "communication/fmt.hpp"
#include "communication/http/session.hpp"
#include "utils/spin_lock.hpp"
#include "utils/synchronized.hpp"

namespace memgraph::communication::http {

template <class TRequestHandler, typename TSessionContext>
class Listener final : public std::enable_shared_from_this<Listener<TRequestHandler, TSessionContext>> {
  using tcp = boost::asio::ip::tcp;
  using SessionHandler = Session<TRequestHandler, TSessionContext>;
  using std::enable_shared_from_this<Listener<TRequestHandler, TSessionContext>>::shared_from_this;

  void LogErrorListener(boost::beast::error_code ec, const std::string_view what) {
    spdlog::warn("HTTP listener failed on {}: {}", what, ec.message());
  }

 public:
  Listener(const Listener &) = delete;
  Listener(Listener &&) = delete;
  Listener &operator=(const Listener &) = delete;
  Listener &operator=(Listener &&) = delete;
  ~Listener() = default;

  template <typename... Args>
  static std::shared_ptr<Listener> Create(Args &&...args) {
    return std::shared_ptr<Listener>{new Listener(std::forward<Args>(args)...)};
  }

  // Start accepting incoming connections
  void Run() { DoAccept(); }

  bool HasErrorHappened() const { return error_happened_.load(std::memory_order_acquire); }

  std::optional<tcp::endpoint> GetEndpoint() const {
    try {
      return acceptor_.local_endpoint();
    } catch (const boost::system::system_error &e) {
      spdlog::error("Failed to get remote endpoint for listener.");
      return std::nullopt;
    }
  }

 private:
  Listener(boost::asio::io_context &ioc, TSessionContext *session_context, ServerContext *context,
           tcp::endpoint endpoint)
      : ioc_(ioc), session_context_(session_context), context_(context), acceptor_(ioc), retry_timer_(ioc) {
    boost::beast::error_code ec;

    // Open the acceptor
    acceptor_.open(endpoint.protocol(), ec);
    if (ec) {
      LogErrorListener(ec, "open");
      error_happened_.store(true, std::memory_order_release);
      return;
    }

    // Allow address reuse
    acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec) {
      LogErrorListener(ec, "set_option");
      error_happened_.store(true, std::memory_order_release);
      return;
    }

    // Bind to the server address
    acceptor_.bind(endpoint, ec);
    if (ec) {
      LogErrorListener(ec, "bind");
      error_happened_.store(true, std::memory_order_release);
      return;
    }

    acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
      LogErrorListener(ec, "listen");
      error_happened_.store(true, std::memory_order_release);
      return;
    }

    spdlog::info("HTTP server is listening on {}", endpoint);
  }

  void DoAccept() {
    acceptor_.async_accept(ioc_, [shared_this = shared_from_this()](auto ec, auto socket) {
      shared_this->OnAccept(ec, std::move(socket));
    });
  }

  void OnAccept(boost::beast::error_code ec, tcp::socket socket) {
    if (ec) {
      if (IsTransientAcceptError(ec)) {
        accept_log_throttle_.Warn("HTTP listener", ec.message());
        RetryAcceptLater(retry_timer_, [shared_this = shared_from_this()] { shared_this->DoAccept(); });
        return;
      }
      if (ec != boost::asio::error::operation_aborted) {
        error_happened_.store(true, std::memory_order_release);
        LogErrorListener(ec, "accept");
      }
      return;
    }

    try {
      SessionHandler::Create(std::move(socket), session_context_, *context_)->Run();
    } catch (const std::exception &e) {
      spdlog::warn("HTTP listener failed to initialize accepted session: {}", e.what());
    }

    if (acceptor_.is_open()) {
      DoAccept();
    }
  }

  boost::asio::io_context &ioc_;
  TSessionContext *session_context_;
  ServerContext *context_;
  tcp::acceptor acceptor_;
  boost::asio::steady_timer retry_timer_;
  AcceptRetryLogThrottle accept_log_throttle_;
  std::atomic<bool> error_happened_{false};
};
}  // namespace memgraph::communication::http
