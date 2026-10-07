//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "../datastores/datastore.h"
#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../plugins/plugin.h"
#include "../types/subscriber.h"

namespace athenasip::api {

// HTTP Digest (RFC 7616) with a subscriber's SIP credentials, for the routes under /api/v1/subscriber/{realm}/.
// The HTTP realm is the SIP realm, so the HA1 the registrar stores is the key: no other secret is kept, and the
// password a phone registers with is the one it signs these requests with.
class SubscriberAuth : public std::enable_shared_from_this<SubscriberAuth> {
 public:
  struct Outcome {
    enum class Kind {
      // The subscriber, authenticated.
      ok,
      // No usable credentials, or wrong ones: answer 401 with `challenges` as WWW-Authenticate values.
      challenge,
      // The realm is not served here: 404.
      no_realm,
      // The store could not be read: 503.
      unavailable,
    };

    Kind kind = Kind::challenge;
    std::shared_ptr<types::Subscriber> subscriber;
    std::vector<std::string> challenges;
  };

  SubscriberAuth(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor);

  // `authorization` is the request's Authorization value; `method` and `target` are the request's own, which the
  // credentials' method and uri must match.
  void check(const std::string& realm_name, const std::string& method, const std::string& target, const std::string& authorization,
             std::function<void(Outcome)> then);

 private:
  void _challenge(const std::string& realm_name, std::uint32_t nonce_expiry, bool stale, std::function<void(Outcome)> then);

  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::shared_ptr<datastores::Datastore> _datastore;
  plugins::Executor _executor;
};

}  // namespace athenasip::api
