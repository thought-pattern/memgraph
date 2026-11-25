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

#ifdef MG_ENTERPRISE

#include "coordination/coordinator_cert_reloader.hpp"
#include "utils/logging.hpp"
#include "utils/on_scope_exit.hpp"

#include <fmt/format.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <spdlog/spdlog.h>
#include <sys/stat.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace memgraph::coordination {

namespace {

auto OpenSslErrorString() -> std::string {
  unsigned long const err = ERR_peek_last_error();
  if (err == 0) return "no OpenSSL error queued";
  std::array<char, 256> buf{};
  ERR_error_string_n(err, buf.data(), buf.size());
  return std::string{buf.data()};
}

auto StatToIdentity(struct stat const &st) -> CoordinatorCertReloader::FileIdentity {
#if defined(__APPLE__)
  return CoordinatorCertReloader::FileIdentity{.valid = true,
                                               .dev = st.st_dev,
                                               .ino = st.st_ino,
                                               .size = st.st_size,
                                               .mtime_sec = static_cast<int64_t>(st.st_mtimespec.tv_sec),
                                               .mtime_nsec = static_cast<int64_t>(st.st_mtimespec.tv_nsec)};
#else
  return CoordinatorCertReloader::FileIdentity{.valid = true,
                                               .dev = st.st_dev,
                                               .ino = st.st_ino,
                                               .size = st.st_size,
                                               .mtime_sec = static_cast<int64_t>(st.st_mtim.tv_sec),
                                               .mtime_nsec = static_cast<int64_t>(st.st_mtim.tv_nsec)};
#endif
}

auto StatPathIdentity(std::string const &path) -> CoordinatorCertReloader::FileIdentity {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0) return {};
  return StatToIdentity(st);
}

auto StatOpenFileIdentity(FILE *fp) -> CoordinatorCertReloader::FileIdentity {
  if (fp == nullptr) return {};
  int const fd = ::fileno(fp);
  if (fd < 0) return {};
  struct stat st{};
  if (::fstat(fd, &st) != 0) return {};
  return StatToIdentity(st);
}

}  // namespace

CoordinatorCertReloader &CoordinatorCertReloader::Instance() {
  static CoordinatorCertReloader instance;
  return instance;
}

auto CoordinatorCertReloader::Register(SSL_CTX *server_ctx, SSL_CTX *client_ctx, utils::TlsConfig cfg)
    -> std::expected<void, utils::SSL_CTX_Error> {
  std::lock_guard const guard{reload_mu_};
  DMG_ASSERT(!registered_.load(std::memory_order_relaxed),
             "CoordinatorCertReloader::Register called more than once; cfg_ must not be mutated after startup.");
  server_ctx_ = server_ctx;
  client_ctx_ = client_ctx;
  cfg_ = std::move(cfg);
  registered_.store(true, std::memory_order_release);

  auto loaded = LoadFromDisk();
  if (!loaded.has_value()) return std::unexpected{loaded.error()};
  current_.store(*loaded, std::memory_order_release);

  SSL_CTX_set_cert_cb(server_ctx_, &CoordinatorCertReloader::CertCb, this);
  SSL_CTX_set_cert_cb(client_ctx_, &CoordinatorCertReloader::CertCb, this);

  spdlog::info(
      "Coordinator cert reloader registered (cert={}, key={}, ca={}).", cfg_.cert_file, cfg_.key_file, cfg_.ca_file);
  return {};
}

auto CoordinatorCertReloader::Prepare() -> std::expected<ReloadPrepared, utils::SSL_CTX_Error> {
  if (!registered_.load(std::memory_order_acquire)) return ReloadPrepared{.next = nullptr};

  auto loaded = LoadFromDisk();
  if (!loaded.has_value()) return std::unexpected{loaded.error()};
  return ReloadPrepared{.next = std::move(*loaded)};
}

void CoordinatorCertReloader::Commit(ReloadPrepared prepared) noexcept {
  if (!prepared.next) return;
  current_.store(std::move(prepared.next), std::memory_order_release);
  spdlog::info("Coordinator TLS committed (cert={}, key={}, ca={}).", cfg_.cert_file, cfg_.key_file, cfg_.ca_file);
}

auto CoordinatorCertReloader::LoadFromDisk() -> std::expected<std::shared_ptr<CertData>, utils::SSL_CTX_Error> {
  auto data = std::make_shared<CertData>();

  // --- Load the leaf cert + chain from cfg_.cert_file (PEM, may have chain).
  FILE *cert_fp = std::fopen(cfg_.cert_file.c_str(), "r");
  if (cert_fp == nullptr) {
    return std::unexpected{utils::SSL_CTX_Error{
        .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_CERT_FILE,
        .msg = fmt::format("Couldn't open cert file '{}': {}", cfg_.cert_file, std::strerror(errno))}};
  }
  auto cert_fp_guard = utils::OnScopeExit{[cert_fp] { (void)std::fclose(cert_fp); }};
  auto const opened_cert_id = StatOpenFileIdentity(cert_fp);
  if (!opened_cert_id.valid) {
    return std::unexpected{utils::SSL_CTX_Error{
        .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_CERT_FILE,
        .msg = fmt::format("Couldn't fstat cert file '{}': {}", cfg_.cert_file, std::strerror(errno))}};
  }

  ERR_clear_error();
  X509 *leaf_raw = PEM_read_X509(cert_fp, nullptr, nullptr, nullptr);
  if (leaf_raw == nullptr) {
    return std::unexpected{utils::SSL_CTX_Error{
        .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_CERT_FILE,
        .msg = fmt::format("Couldn't parse leaf cert from '{}': {}", cfg_.cert_file, OpenSslErrorString())}};
  }
  data->leaf.reset(leaf_raw);

  // Drain any intermediate certs that follow the leaf in the same PEM file, rejecting damaged intermediate blocks.
  while (true) {
    ERR_clear_error();
    X509 *next = PEM_read_X509(cert_fp, nullptr, nullptr, nullptr);
    if (next == nullptr) {
      unsigned long const err = ERR_peek_last_error();
      bool const clean_eof = std::feof(cert_fp) != 0 && std::ferror(cert_fp) == 0 &&
                             (err == 0 || (ERR_GET_LIB(err) == ERR_LIB_PEM && ERR_GET_REASON(err) == PEM_R_NO_START_LINE));
      if (!clean_eof) {
        auto const detail = OpenSslErrorString();
        ERR_clear_error();
        return std::unexpected{utils::SSL_CTX_Error{
            .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_CERT_FILE,
            .msg = fmt::format("Couldn't parse certificate chain from '{}': {}", cfg_.cert_file, detail)}};
      }
      ERR_clear_error();
      break;
    }
    data->chain.emplace_back(next);
  }

  // --- Load the private key from cfg_.key_file.
  FILE *key_fp = std::fopen(cfg_.key_file.c_str(), "r");
  if (key_fp == nullptr) {
    return std::unexpected{utils::SSL_CTX_Error{
        .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_KEY_FILE,
        .msg = fmt::format("Couldn't open key file '{}': {}", cfg_.key_file, std::strerror(errno))}};
  }
  auto key_fp_guard = utils::OnScopeExit{[key_fp] { (void)std::fclose(key_fp); }};
  auto const opened_key_id = StatOpenFileIdentity(key_fp);
  if (!opened_key_id.valid) {
    return std::unexpected{utils::SSL_CTX_Error{
        .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_KEY_FILE,
        .msg = fmt::format("Couldn't fstat key file '{}': {}", cfg_.key_file, std::strerror(errno))}};
  }

  ERR_clear_error();
  EVP_PKEY *key_raw = PEM_read_PrivateKey(key_fp, nullptr, nullptr, nullptr);
  if (key_raw == nullptr) {
    return std::unexpected{utils::SSL_CTX_Error{
        .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_KEY_FILE,
        .msg = fmt::format("Couldn't parse private key from '{}': {}", cfg_.key_file, OpenSslErrorString())}};
  }
  data->key.reset(key_raw);

  // Verify the leaf certificate and private key match before publishing this candidate snapshot.
  ERR_clear_error();
  if (X509_check_private_key(data->leaf.get(), data->key.get()) != 1) {
    auto const detail = OpenSslErrorString();
    ERR_clear_error();
    return std::unexpected{utils::SSL_CTX_Error{
        .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_KEY_FILE,
        .msg = fmt::format(
            "Private key '{}' does not match certificate '{}': {}", cfg_.key_file, cfg_.cert_file, detail)}};
  }

  // --- Build the CA verify store from the open descriptor of cfg_.ca_file.
  FILE *ca_fp = std::fopen(cfg_.ca_file.c_str(), "r");
  if (ca_fp == nullptr) {
    return std::unexpected{utils::SSL_CTX_Error{
        .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_LOAD_CA,
        .msg = fmt::format("Couldn't open CA file '{}': {}", cfg_.ca_file, std::strerror(errno))}};
  }
  auto ca_fp_guard = utils::OnScopeExit{[ca_fp] { (void)std::fclose(ca_fp); }};
  auto const opened_ca_id = StatOpenFileIdentity(ca_fp);
  if (!opened_ca_id.valid) {
    return std::unexpected{utils::SSL_CTX_Error{
        .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_LOAD_CA,
        .msg = fmt::format("Couldn't fstat CA file '{}': {}", cfg_.ca_file, std::strerror(errno))}};
  }

  X509_STORE *store_raw = X509_STORE_new();
  if (store_raw == nullptr) {
    return std::unexpected{utils::SSL_CTX_Error{.err_type = utils::SSL_CTX_ERR_TYPE::FAIL_LOAD_CA,
                                                .msg = "X509_STORE_new returned nullptr"}};
  }
  data->verify_store.reset(store_raw);

  ERR_clear_error();
  STACK_OF(X509_INFO) *ca_infos = PEM_X509_INFO_read(ca_fp, nullptr, nullptr, nullptr);
  if (ca_infos == nullptr) {
    auto const detail = OpenSslErrorString();
    ERR_clear_error();
    return std::unexpected{utils::SSL_CTX_Error{
        .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_LOAD_CA,
        .msg = fmt::format("Couldn't load CA from '{}': {}", cfg_.ca_file, detail)}};
  }
  auto ca_infos_guard = utils::OnScopeExit{[ca_infos] { sk_X509_INFO_pop_free(ca_infos, X509_INFO_free); }};

  int added_ca_entries = 0;
  for (int i = 0; i < sk_X509_INFO_num(ca_infos); ++i) {
    X509_INFO *inf = sk_X509_INFO_value(ca_infos, i);
    if (inf->x509 != nullptr) {
      if (X509_STORE_add_cert(store_raw, inf->x509) != 1) {
        unsigned long const add_err = ERR_peek_last_error();
        if (ERR_GET_REASON(add_err) != X509_R_CERT_ALREADY_IN_HASH_TABLE) {
          auto const detail = OpenSslErrorString();
          ERR_clear_error();
          return std::unexpected{utils::SSL_CTX_Error{
              .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_LOAD_CA,
              .msg = fmt::format("Couldn't add CA cert from '{}': {}", cfg_.ca_file, detail)}};
        }
        ERR_clear_error();
      }
      ++added_ca_entries;
    }
    if (inf->crl != nullptr) {
      if (X509_STORE_add_crl(store_raw, inf->crl) != 1) {
        unsigned long const add_err = ERR_peek_last_error();
        if (ERR_GET_REASON(add_err) != X509_R_CERT_ALREADY_IN_HASH_TABLE) {
          auto const detail = OpenSslErrorString();
          ERR_clear_error();
          return std::unexpected{utils::SSL_CTX_Error{
              .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_LOAD_CA,
              .msg = fmt::format("Couldn't add CRL from '{}': {}", cfg_.ca_file, detail)}};
        }
        ERR_clear_error();
      }
      ++added_ca_entries;
    }
  }
  if (added_ca_entries == 0 || std::ferror(ca_fp) != 0) {
    return std::unexpected{utils::SSL_CTX_Error{
        .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_LOAD_CA,
        .msg = fmt::format("No valid CA entries loaded from '{}'", cfg_.ca_file)}};
  }

  // Re-verify path identities and mtimes across the read so a concurrent replacement never stamps older bytes with
  // newer metadata.
  if (StatOpenFileIdentity(cert_fp) != opened_cert_id || StatPathIdentity(cfg_.cert_file) != opened_cert_id) {
    return std::unexpected{utils::SSL_CTX_Error{
        .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_CERT_FILE,
        .msg = fmt::format("Certificate file '{}' changed concurrently during reload", cfg_.cert_file)}};
  }
  if (StatOpenFileIdentity(key_fp) != opened_key_id || StatPathIdentity(cfg_.key_file) != opened_key_id) {
    return std::unexpected{utils::SSL_CTX_Error{
        .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_KEY_FILE,
        .msg = fmt::format("Private key file '{}' changed concurrently during reload", cfg_.key_file)}};
  }
  if (StatOpenFileIdentity(ca_fp) != opened_ca_id || StatPathIdentity(cfg_.ca_file) != opened_ca_id) {
    return std::unexpected{utils::SSL_CTX_Error{
        .err_type = utils::SSL_CTX_ERR_TYPE::FAIL_LOAD_CA,
        .msg = fmt::format("CA file '{}' changed concurrently during reload", cfg_.ca_file)}};
  }

  data->cert_id = opened_cert_id;
  data->key_id = opened_key_id;
  data->ca_id = opened_ca_id;

  return data;
}

auto CoordinatorCertReloader::RefreshIfStale() -> std::expected<void, utils::SSL_CTX_Error> {
  auto snapshot = current_.load(std::memory_order_acquire);

  auto const cert_id_now = StatPathIdentity(cfg_.cert_file);
  auto const key_id_now = StatPathIdentity(cfg_.key_file);
  auto const ca_id_now = StatPathIdentity(cfg_.ca_file);

  bool const stale = !snapshot ||                     //
                     cert_id_now != snapshot->cert_id ||  //
                     key_id_now != snapshot->key_id ||    //
                     ca_id_now != snapshot->ca_id;
  if (!stale) return {};

  std::lock_guard const guard{reload_mu_};
  snapshot = current_.load(std::memory_order_acquire);
  if (snapshot && cert_id_now == snapshot->cert_id && key_id_now == snapshot->key_id && ca_id_now == snapshot->ca_id) {
    return {};
  }

  auto loaded = LoadFromDisk();
  if (!loaded.has_value()) return std::unexpected{loaded.error()};

  current_.store(*loaded, std::memory_order_release);
  spdlog::info("Coordinator TLS reloaded via lazy file-identity check (cert={}, key={}, ca={}).",
               cfg_.cert_file,
               cfg_.key_file,
               cfg_.ca_file);
  return {};
}

int CoordinatorCertReloader::CertCb(SSL *ssl, void *arg) {
  auto *self = static_cast<CoordinatorCertReloader *>(arg);

  if (auto r = self->RefreshIfStale(); !r.has_value()) {
    spdlog::error("Coordinator cert reload failed during handshake: {}", r.error().msg);
  }

  auto snapshot = self->current_.load(std::memory_order_acquire);
  if (!snapshot) {
    spdlog::error("Coordinator cert reloader has no snapshot during handshake — aborting.");
    return 0;
  }
  return ApplyToSsl(ssl, *snapshot);
}

int CoordinatorCertReloader::ApplyToSsl(SSL *ssl, CertData const &data) {
  if (SSL_clear_chain_certs(ssl) != 1) {
    spdlog::error("SSL_clear_chain_certs failed: {}", OpenSslErrorString());
    return 0;
  }
  if (SSL_use_certificate(ssl, data.leaf.get()) != 1) {
    spdlog::error("SSL_use_certificate failed: {}", OpenSslErrorString());
    return 0;
  }
  for (auto const &intermediate : data.chain) {
    if (SSL_add1_chain_cert(ssl, intermediate.get()) != 1) {
      spdlog::error("SSL_add1_chain_cert failed: {}", OpenSslErrorString());
      return 0;
    }
  }
  if (SSL_use_PrivateKey(ssl, data.key.get()) != 1) {
    spdlog::error("SSL_use_PrivateKey failed: {}", OpenSslErrorString());
    return 0;
  }
  if (data.verify_store && SSL_set1_verify_cert_store(ssl, data.verify_store.get()) != 1) {
    spdlog::error("SSL_set1_verify_cert_store failed: {}", OpenSslErrorString());
    return 0;
  }
  return 1;
}

}  // namespace memgraph::coordination

#endif  // MG_ENTERPRISE
