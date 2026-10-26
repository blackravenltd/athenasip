//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "subscriber_auth.h"

#include <ctime>
#include <utility>

#include "../digest.h"
#include "../types/authorization.h"
#include "../types/realm.h"
#include "../types/sip_identity.h"
#include "../util.h"

namespace athenasip::api {

SubscriberAuth::SubscriberAuth(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor)
    : _logger(std::make_shared<loggers::LoggerScoped>("subscriber_auth", std::move(logger))),
      _datastore(std::move(datastore)),
      _executor(std::move(executor)) {}

void SubscriberAuth::check(const std::string& realm_name, const std::string& method, const std::string& target, const std::string& authorization,
                           std::function<void(Outcome)> then) {
  auto self = shared_from_this();

  _datastore->realm_get_by_name(
      _executor, Util::to_lower(realm_name), [this, self, method, target, authorization, then](plugins::Result<std::shared_ptr<types::Realm>> found) {
        if (!found.ok) return then(Outcome{Outcome::Kind::unavailable, nullptr, {}});
        if (!found.value) return then(Outcome{Outcome::Kind::no_realm, nullptr, {}});

        const auto realm = found.value;
        auto credentials = std::make_shared<types::Authorization>(authorization);

        // RFC 7616 3.4: Digest, for this realm, with a username to look up.
        const bool usable = Util::to_lower(credentials->type) == "digest" && digest::is_complete(credentials) && credentials->contains_field("username") &&
                            credentials->fields["realm"] == realm->name;
        if (!usable) return _challenge(*realm, false, then);

        // The nonce carries its own proof and its minting time (digest::nonce_mint): nothing is looked up.
        switch (digest::nonce_state(*realm, credentials->fields["nonce"], "")) {
          case digest::NonceState::bad:
            return _challenge(*realm, false, then);
          case digest::NonceState::stale:
            // RFC 7616 3.3: the client signs again without asking its user.
            return _challenge(*realm, true, then);
          case digest::NonceState::good:
            break;
        }

        auto identity = std::make_shared<types::SIPIdentity>("sip:" + credentials->fields["username"] + "@" + realm->name);
        _datastore->subscriber_get(
            _executor, identity, [this, self, method, target, credentials, realm, then](plugins::Result<std::shared_ptr<types::Subscriber>> found) {
              if (!found.ok) return then(Outcome{Outcome::Kind::unavailable, nullptr, {}});

              // An unknown subscriber is challenged like a wrong password, so subscribers cannot be enumerated.
              if (!found.value) return _challenge(*realm, false, then);

              // The uri the client signed is this request's target (RFC 7616 3.4.6).
              if (const auto why = digest::verify(*found.value, *credentials, method, target); !why.empty()) {
                _logger->info("Subscriber request for " + found.value->identity->to_string() + " with " + why + " - challenging");
                return _challenge(*realm, false, then);
              }

              // A right answer seen before is a replay (RFC 7616 5.6): stale, so a client whose nc slipped signs again.
              if (!_counts.accept(*credentials, std::time(nullptr) + realm->nonce_expiry)) {
                _logger->info("Subscriber request for " + found.value->identity->to_string() + " replays an answer - challenging as stale");
                return _challenge(*realm, true, then);
              }

              then(Outcome{Outcome::Kind::ok, found.value, {}});
            });
      });
}

void SubscriberAuth::_challenge(const types::Realm& realm, bool stale, std::function<void(Outcome)> then) {
  const auto nonce = digest::nonce_mint(realm, "");

  // RFC 7616 3.3: SHA-256 first, then MD5 for clients that have only that, each with qop=auth.
  Outcome outcome{Outcome::Kind::challenge, nullptr, {}};
  for (const std::string algorithm : {"SHA-256", "MD5"}) {
    outcome.challenges.push_back("Digest realm=\"" + realm.name + "\", qop=\"auth\", algorithm=" + algorithm + ", nonce=\"" + nonce + "\"" +
                                 (stale ? ", stale=true" : ""));
  }
  then(std::move(outcome));
}

}  // namespace athenasip::api
