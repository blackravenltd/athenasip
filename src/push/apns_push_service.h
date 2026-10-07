//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../types/url.h"
#include "http2_client.h"
#include "jwt.h"
#include "push_service.h"

namespace athenasip::push {

// Apple Push Notification service, as RFC 8599 section 10 uses it: pn-param is the Team ID
// and the topic separated by the first period, the topic being the bundle ID with a
// service suffix (".voip" for PushKit), and pn-prid is the device token. The node
// authenticates with a token signed by the team's APNs key (ES256, the .p8 file Apple
// issues) and sends over HTTP/2, which is all APNs speaks.
//
//   push:
//     apns:
//       key_file: /etc/athenasip/AuthKey_ABC123DEFG.p8  # the key Apple issues
//       key_id: ABC123DEFG                              # its key ID
//       team_id: DEF123GHIJ                             # the team the app belongs to
//       environment: production                         # or sandbox, for development builds
//       ttl: 60                                         # seconds, optional
//       api_base: https://api.push.apple.com            # optional
//       ca_file: /etc/athenasip/private-ca.pem          # optional
class ApnsPushService final : public PushService, public std::enable_shared_from_this<ApnsPushService> {
 public:
  using Clock = std::function<std::chrono::system_clock::time_point()>;

  ApnsPushService(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url);

  std::string name() const override;

  // The section named after this driver, for the reference and the misspelt-key check.

  static plugins::Settings settings();
  std::string version() const override;

  bool configure(const YAML::Node& own_root, const Config& system) override;
  bool health() const override;
  void close() override;

  bool accepts(const Notification& notification) const override;

  bool refreshes(const Notification& notification) const override;
  void send(plugins::Executor on, Notification notification, plugins::StatusHandler handler) override;

  // The wall clock a token's iat and apns-expiration are read from. Tests replace it.
  void set_clock(Clock clock);

 private:
  struct Token {
    std::string value;
    std::chrono::system_clock::time_point issued;
  };

  Token provider_token(std::chrono::system_clock::time_point now);
  void provider_token_expired(const Token& token, std::chrono::system_clock::time_point now);

  std::shared_ptr<loggers::Logger> _base_logger;
  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::shared_ptr<types::URL> _url;

  jwt::Key _key;
  std::string _key_id;
  std::string _team_id;
  std::string _api_base;
  long _ttl = 60;
  std::shared_ptr<Http2Client> _client;
  Clock _clock = [] { return std::chrono::system_clock::now(); };

  std::mutex _mutex;
  Token _token;
};

}  // namespace athenasip::push
