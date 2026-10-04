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
#include <vector>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../types/url.h"
#include "https_client.h"
#include "jwt.h"
#include "push_service.h"

namespace athenasip::push {

// Firebase Cloud Messaging, as RFC 8599 section 11 uses it: pn-param is the Firebase
// project ID and pn-prid the registration token. The node authenticates as a Google
// service account, exchanging a signed JWT for an OAuth 2.0 access token (RFC 7523) which
// it keeps until shortly before it expires, and sends a high-priority data message
// through the HTTP v1 API.
//
//   push:
//     fcm:
//       service_account: /etc/athenasip/firebase.json  # the key file Google issues
//       ttl: 60                                         # seconds, optional
//       api_base: https://fcm.googleapis.com            # optional
//       ca_file: /etc/athenasip/private-ca.pem          # optional
class FcmPushService final : public PushService, public std::enable_shared_from_this<FcmPushService> {
 public:
  FcmPushService(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url);

  std::string name() const override;
  std::string version() const override;

  bool configure(const YAML::Node& own_root, const Config& system) override;
  bool health() const override;

  bool accepts(const Notification& notification) const override;
  void send(plugins::Executor on, Notification notification, plugins::StatusHandler handler) override;

 private:
  using TokenHandler = std::function<void(plugins::Result<std::string>)>;

  void with_access_token(TokenHandler handler);
  void fetch_access_token();
  void token_arrived(plugins::Result<std::string> token, std::chrono::seconds lifetime);
  void forget_access_token(const std::string& token);
  void deliver(plugins::Executor on, const Notification& notification, const std::string& token, plugins::StatusHandler handler);

  std::shared_ptr<loggers::Logger> _base_logger;
  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::shared_ptr<types::URL> _url;

  jwt::Key _key;
  std::string _key_id;
  std::string _client_email;
  std::string _token_uri;
  std::string _api_base = "https://fcm.googleapis.com";
  long _ttl = 60;
  std::shared_ptr<HttpsClient> _client;
  plugins::Executor _strand;

  std::mutex _mutex;
  std::string _access_token;
  std::chrono::steady_clock::time_point _refresh_at;
  bool _fetching = false;
  std::vector<TokenHandler> _waiting;
};

}  // namespace athenasip::push
