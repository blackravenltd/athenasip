//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../types/url.h"
#include "https_client.h"
#include "jwt.h"
#include "push_service.h"

namespace athenasip::push {

// Generic Event Delivery Using HTTP Push (RFC 8030), as RFC 8599 section 12 uses it: the
// binding's pn-prid is the push subscription URI, and a push is a POST to it with no
// payload, so RFC 8291 encryption never applies. The node identifies itself with VAPID
// (RFC 8292), and advertises the key in +sip.vapid so a UA can restrict its subscription
// to this server.
//
//   push:
//     webpush:
//       vapid_private_key: /etc/athenasip/vapid.pem   # P-256, PEM
//       subject: mailto:ops@example.com               # or an https: URI
//       ttl: 60                                       # seconds, optional
//       ca_file: /etc/athenasip/private-ca.pem        # optional
class WebpushPushService final : public PushService {
 public:
  WebpushPushService(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url);

  std::string name() const override;
  std::string version() const override;

  bool configure(const YAML::Node& own_root, const Config& system) override;
  bool health() const override;

  bool accepts(const Notification& notification) const override;
  std::vector<std::pair<std::string, std::string>> capabilities() const override;
  void send(plugins::Executor on, Notification notification, plugins::StatusHandler handler) override;

  // RFC 6454 6.1: the serialisation of a URI's origin, which is what a VAPID token's
  // "aud" names. Empty when the URI is not an https URI with a host.
  static std::string origin(const std::string& uri);

 private:
  std::shared_ptr<loggers::Logger> _base_logger;
  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::shared_ptr<types::URL> _url;

  jwt::Key _key;
  std::string _public_key;  // base64url of the uncompressed point, as "k" and +sip.vapid carry it
  std::string _subject;
  long _ttl = 60;
  std::shared_ptr<HttpsClient> _client;
};

}  // namespace athenasip::push
