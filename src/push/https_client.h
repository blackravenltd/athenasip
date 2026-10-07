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
#include <string_view>
#include <utility>
#include <vector>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../plugins/plugin.h"

namespace athenasip::push {

using HttpsHeaders = std::vector<std::pair<std::string, std::string>>;

struct HttpsResponse {
  unsigned status = 0;
  HttpsHeaders headers;
  std::string body;

  // The first value of a header field, compared without case; empty when absent.
  std::string header(std::string_view name) const;
};

struct HttpsClientOptions {
  // Trusted beside the system's roots: a private CA in front of a push service, or a test's.
  std::string ca_file;
  // From the start of resolution to the last byte of the response.
  std::chrono::milliseconds timeout{std::chrono::seconds(10)};
};

// The push services' HTTP: one request and one response per connection, over TLS with
// the peer's certificate verified. A push is rare enough that a pooled connection would
// be complexity for nothing, and HTTP/1.1 is what Web Push and FCM both accept.
class HttpsClient {
 public:
  using Options = HttpsClientOptions;

  explicit HttpsClient(std::shared_ptr<loggers::Logger> logger, Options options = {});

  // Empty when the client can be used; otherwise why not (a CA file that would not load).
  const std::string& error() const { return _error; }

  // POSTs body to an https URL. The handler is posted to `on`; it fails only when there is
  // no response at all, and a response of any status is a success carrying that status.
  void post(plugins::Executor on, const std::string& url, HttpsHeaders headers, std::string body, plugins::Handler<HttpsResponse> handler);

 private:
  std::shared_ptr<loggers::LoggerScoped> _logger;
  Options _options;
  std::shared_ptr<boost::asio::ssl::context> _context;
  std::string _error;
};

}  // namespace athenasip::push
