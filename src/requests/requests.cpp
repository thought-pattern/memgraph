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

#include "requests/requests.hpp"

#include <curl/curl.h>
#include <curl/system.h>
#include <fmt/format.h>
#include <gflags/gflags.h>
#include <array>
#include <compare>
#include <cstdio>
#include <ctre.hpp>
#include <exception>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <sstream>
#include <utility>

#include "spdlog/spdlog.h"
#include "utils/counter.hpp"
#include "utils/exceptions.hpp"
#include "utils/on_scope_exit.hpp"
#include "utils/string.hpp"

namespace memgraph::requests {

auto RedactUrlForLog(std::string_view url) -> std::string {
  if (url.empty()) {
    return "<empty-url>";
  }
  const auto scheme_pos = url.find("://");
  std::string out;
  std::string_view rest = url;
  if (scheme_pos != std::string_view::npos) {
    out.append(url.substr(0, scheme_pos + 3));
    rest = url.substr(scheme_pos + 3);
  }
  // Authority ends at the first '/', '?', or '#'.
  const auto auth_end = rest.find_first_of("/?#");
  const std::string_view authority = auth_end == std::string_view::npos ? rest : rest.substr(0, auth_end);
  const std::string_view path_and_more = auth_end == std::string_view::npos ? std::string_view{} : rest.substr(auth_end);

  const auto at_pos = authority.rfind('@');
  const std::string_view host_port = at_pos == std::string_view::npos ? authority : authority.substr(at_pos + 1);
  out.append(host_port);

  const auto query_or_frag = path_and_more.find_first_of("?#");
  const std::string_view clean_path =
      query_or_frag == std::string_view::npos ? path_and_more : path_and_more.substr(0, query_or_frag);
  out.append(clean_path);
  return out.empty() ? std::string{"<redacted-url>"} : out;
}

namespace {

struct ProgressData {
  std::function<void()> abort_check_;
  // Why the caller wanted to stop. Kept so it can be rethrown once the transfer has unwound, since
  // an exception must not cross libcurl's frames.
  std::exception_ptr abort_error_;
};

// Callback function for reporting progress during a file download
auto DownloadProgressCb(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t /*ultotal*/,
                        curl_off_t /*ulnow*/) -> int {
  constexpr auto kAbortTransferReturnCode = 1;
  constexpr auto kContinueTransferReturnCode = 0;

  auto *data = static_cast<ProgressData *>(clientp);

  // Ahead of anything that needs a total: a transfer whose length the server never announces still
  // has to be stoppable, and a chunked response never reports one.
  // abort_check_ could be a nullptr.
  if (data->abort_check_) {
    try {
      data->abort_check_();
    } catch (...) {
      data->abort_error_ = std::current_exception();
      return kAbortTransferReturnCode;
    }
  }

  // Only the progress figure needs a total to mean anything.
  if (dltotal != 0) {
    static thread_local auto counter = utils::ResettableCounter(500);

    // Don't log too often but log when the file download is complete
    if (counter() || dlnow == dltotal) {
      auto const progress = (100.0F * static_cast<float>(dlnow)) / static_cast<float>(dltotal);
      spdlog::trace("Downloaded {:.2f}% of the file", progress);
    }
  }

  return kContinueTransferReturnCode;
}

size_t CurlWriteCallback(char * /*ptr*/, size_t /*size*/, size_t nmemb, void * /*userdata*/) { return nmemb; }

// Progress callback for POST requests - checks per-request abort flag
auto PostProgressCallback(void *clientp, curl_off_t /*dltotal*/, curl_off_t /*dlnow*/, curl_off_t /*ultotal*/,
                          curl_off_t /*ulnow*/) -> int {
  auto const *abort_flag = static_cast<std::atomic<bool> const *>(clientp);
  if (abort_flag && abort_flag->load(std::memory_order_relaxed)) {
    return 1;  // Return non-zero to abort transfer
  }
  return 0;  // Continue transfer
}

// libcurl's default CA bundle path is a build-time constant, wrong on distros
// other than the build host's; use the path given here if there is one,
// otherwise probe the standard locations.
//
// Resolved on the first call and fixed for the process lifetime, which is why
// Init() must call it before anything issues a request: the path it was given
// is only read on that first call.
const char *ResolveCaBundle(std::string_view ca_bundle_file = {}) {
  static const std::string resolved = [ca_bundle_file]() -> std::string {
    // An explicit path is passed through unvalidated so a bad value fails
    // loudly (CURLE_SSL_CACERT_BADFILE) instead of silently falling back.
    if (!ca_bundle_file.empty()) return std::string{ca_bundle_file};
    constexpr std::array paths = {
        "/etc/ssl/certs/ca-certificates.crt",      // Debian/Ubuntu
        "/etc/pki/tls/certs/ca-bundle.crt",        // RHEL/Fedora/CentOS/Rocky
        "/etc/ssl/ca-bundle.pem",                  // SUSE
        "/usr/local/share/certs/ca-root-nss.crt",  // FreeBSD (ca_root_nss port)
        "/etc/ssl/cert.pem",                       // OpenBSD, Alpine and others
    };
    for (const auto *path : paths) {
      std::error_code ec;
      if (std::filesystem::is_regular_file(path, ec)) return path;
    }
    spdlog::warn(
        "No system CA bundle found in the standard locations; https requests may fail certificate verification. Set "
        "--ca-bundle-file to fix this.");
    return "";  // keep libcurl's compiled-in default
  }();
  return resolved.empty() ? nullptr : resolved.c_str();
}

void SetCaInfo(CURL *curl) {
  if (const char *bundle = ResolveCaBundle(); bundle != nullptr) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, bundle);
  }
}

}  // namespace

void Init(std::string_view ca_bundle_file) {
  ResolveCaBundle(ca_bundle_file);
  curl_global_init(CURL_GLOBAL_ALL);
}

bool RequestPostJson(const std::string &url, const nlohmann::json &data, int timeout_in_seconds,
                     std::atomic<bool> const *abort_flag) {
  CURL *curl = nullptr;
  CURLcode res = CURLE_UNSUPPORTED_PROTOCOL;

  long response_code = 0;  // NOLINT(google-runtime-int) - curl_easy_getinfo requires long*
  struct curl_slist *headers = nullptr;
  std::string payload = data.dump();
  std::string user_agent = fmt::format("memgraph/{}", gflags::VersionString());

  curl = curl_easy_init();
  if (!curl) return false;

  headers = curl_slist_append(headers, "Accept: application/json");
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "charsets: utf-8");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_in_seconds);
  curl_easy_setopt(curl, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));  // NOLINT(google-runtime-int)
  SetCaInfo(curl);

  // Enable progress callback so an in-flight transfer can be aborted
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, abort_flag);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, PostProgressCallback);

  res = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);

  if (res != CURLE_OK) {
    SPDLOG_WARN("Couldn't perform request: {}", curl_easy_strerror(res));
    return false;
  }

  if (response_code < 200 || response_code >= 300) {
    SPDLOG_WARN("Request response code isn't 2xx (received {})!", response_code);
    return false;
  }

  return true;
}

std::expected<void, DownloadError> CreateAndDownloadFile(const std::string &url, utils::FileUniquePtr file,
                                                         uint64_t const connection_timeout,
                                                         std::function<void()> abort_check) {
  if (!file) {
    return std::unexpected{
        DownloadError{.kind = DownloadFailure::LocalWrite, .http_status = 0, .message = "the download could not be written"}};
  }
  // A short write ends the transfer, which is what a full disk should do.
  auto const sink = [&file](char const *data, size_t const size) -> size_t {
    return std::fwrite(data, 1, size, file.get());
  };

  auto result = DownloadToSink(url, sink, connection_timeout, std::move(abort_check));
  FILE *raw_file = file.release();
  const int flush_rc = std::fflush(raw_file);
  const int stream_err = std::ferror(raw_file);
  const int close_rc = std::fclose(raw_file);
  if (result.has_value() && (flush_rc != 0 || stream_err != 0 || close_rc != 0)) {
    return std::unexpected{
        DownloadError{.kind = DownloadFailure::LocalWrite, .http_status = 0, .message = "the download could not be written"}};
  }
  return result;
}

std::expected<void, DownloadError> DownloadToSink(const std::string &url, WriteSink const &write,
                                                  uint64_t const connection_timeout, std::function<void()> abort_check,
                                                  uint64_t const stall_window_sec) {
  auto const user_agent = fmt::format("memgraph/{}", gflags::VersionString());

  auto *curl = curl_easy_init();
  if (!curl) {
    spdlog::error("requests: Couldn't init curl");
    return std::unexpected{DownloadError{.kind = DownloadFailure::Configuration, .message = "could not start a request"}};
  }
  utils::OnScopeExit const cleanup{[curl]() { curl_easy_cleanup(curl); }};

  ProgressData progress_data{.abort_check_ = std::move(abort_check)};

  constexpr auto write_callback = [](char *ptr, size_t size, size_t nmemb, void *userdata) -> size_t {
    auto const *sink = static_cast<WriteSink const *>(userdata);
    return (*sink)(ptr, size * nmemb);
  };

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write);
  // Timeout for establishing a connection
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connection_timeout);
  // Give up on a body that stops arriving. libcurl decides this from the bytes the server actually
  // delivers, so a sink that blocks while its reader catches up does not look like a stall.
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>(stall_window_sec));  // NOLINT(google-runtime-int)
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "GET");
  curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10);
  // Needed so that XFERINFOFUNCTION could work
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_data);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, DownloadProgressCb);
  // Fail fast on HTTP errors, so an error page is never handed to the sink
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  // TLS below 1.2 is deprecated. Stating the floor here keeps it independent of what the TLS backend
  // this is built against happens to permit.
  curl_easy_setopt(curl, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));  // NOLINT(google-runtime-int)
  SetCaInfo(curl);

  auto const res = curl_easy_perform(curl);
  if (res == CURLE_OK) {
    return {};
  }

  // The caller stopped this deliberately, so report what it asked for rather than a download failure.
  if (progress_data.abort_error_) {
    std::rethrow_exception(progress_data.abort_error_);
  }

  long status = 0;  // NOLINT(google-runtime-int) - curl_easy_getinfo requires long*
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  auto error = DownloadError{.http_status = static_cast<int>(status), .message = curl_easy_strerror(res)};
  switch (res) {
    case CURLE_HTTP_RETURNED_ERROR:
      error.kind = ClassifyHttpStatus(static_cast<int>(status));
      error.message = fmt::format("the server replied {}", status);
      break;
    case CURLE_WRITE_ERROR:
      error.kind = DownloadFailure::LocalWrite;
      error.message = "the download could not be written";
      break;
    case CURLE_OPERATION_TIMEDOUT:
      error.kind = DownloadFailure::Stalled;
      error.message = "the transfer stopped making progress";
      break;
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_URL_MALFORMAT:
    case CURLE_NOT_BUILT_IN:
    case CURLE_BAD_FUNCTION_ARGUMENT:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_CERTPROBLEM:
      error.kind = DownloadFailure::Configuration;
      break;
    default:
      error.kind = DownloadFailure::Network;
      break;
  }

  spdlog::error("Error happened while downloading {}: {}", RedactUrlForLog(url), error.message);
  return std::unexpected{std::move(error)};
}

auto DownloadToStream(std::string_view url, std::ostream &os) -> bool {
  if (!os.good()) {
    return false;
  }
  constexpr auto WriteCallback = [](char *ptr, size_t size, size_t nmemb, std::ostream *os) -> size_t {
    const size_t byte_count = size * nmemb;
    if (byte_count == 0) {
      return 0;
    }
    if (!os->good()) {
      return 0;
    }
    os->write(ptr, static_cast<std::streamsize>(byte_count));
    if (!(*os)) {
      return 0;
    }
    return byte_count;
  };

  auto *curl_handle{curl_easy_init()};
  if (!curl_handle) {
    return false;
  }
  const std::string owned_url{url};
  curl_easy_setopt(curl_handle, CURLOPT_URL, owned_url.c_str());
  curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, +WriteCallback);
  curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, &os);
  curl_easy_setopt(curl_handle, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl_handle, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl_handle, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));  // NOLINT(google-runtime-int)
  SetCaInfo(curl_handle);

  auto const res = curl_easy_perform(curl_handle);
  long response_code = 0;  // NOLINT
  curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &response_code);
  curl_easy_cleanup(curl_handle);

  if (res != CURLE_OK || !os.good()) {
    if (res != CURLE_OK) {
      SPDLOG_WARN("Couldn't perform request: {}", curl_easy_strerror(res));
    }
    return false;
  }

  const auto scheme_sep = owned_url.find("://");
  if (scheme_sep != std::string::npos) {
    const std::string scheme_lower = utils::ToLowerCase(std::string_view{owned_url}.substr(0, scheme_sep));
    if (scheme_lower == "http" || scheme_lower == "https" || scheme_lower == "ftp" || scheme_lower == "ftps") {
      if (response_code < 200 || response_code >= 300) {
        SPDLOG_WARN("Request response code isn't 2xx (received {})!", response_code);
        return false;
      }
    }
  }

  return true;
}

auto UrlToStringStream(std::string_view url) -> std::stringstream {
  auto ss = std::stringstream{};
  if (!requests::DownloadToStream(url, ss)) {
    throw utils::RetryableException("CSV was unable to be fetched from {}", RedactUrlForLog(url));
  }
  return ss;
};

}  // namespace memgraph::requests
