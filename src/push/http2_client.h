//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio/ssl/context.hpp>
#include <chrono>
#include <memory>
#include <string>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../plugins/plugin.h"
#include "https_client.h"

namespace athenasip::push {

struct Http2ClientOptions {
  // Trusted beside the system's roots: a private CA in front of a push service, or a test's.
  std::string ca_file;
  // From the moment a request is made to the last byte of its response.
  std::chrono::milliseconds timeout{std::chrono::seconds(10)};
};

// HTTP/2 over TLS (RFC 9113), for the push services that speak nothing else: APNs. The
// server must agree to "h2" by ALPN and its certificate is verified. One connection per
// origin is kept open and carries every request to it, as Apple asks; a new one is opened
// when the last has gone. Only what a push needs: a POST and its response.
class Http2Client {
 public:
  using Options = Http2ClientOptions;

  explicit Http2Client(std::shared_ptr<loggers::Logger> logger, Options options = {});
  ~Http2Client();

  Http2Client(const Http2Client&) = delete;
  Http2Client& operator=(const Http2Client&) = delete;

  // Empty when the client can be used; otherwise why not (a CA file that would not load).
  const std::string& error() const { return _error; }

  // POSTs body to an https URL. The handler is posted to `on`; it fails only when there is
  // no response at all, and a response of any status is a success carrying that status.
  void post(plugins::Executor on, const std::string& url, HttpsHeaders headers, std::string body, plugins::Handler<HttpsResponse> handler);

  // Closes every connection; requests still waiting fail.
  void close();

 private:
  class Connection;
  class Pool;

  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::chrono::milliseconds _timeout;
  std::string _error;
  std::shared_ptr<Pool> _pool;
};

}  // namespace athenasip::push
