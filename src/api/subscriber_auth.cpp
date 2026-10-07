//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "subscriber_auth.h"

#include <openssl/rand.h>

#include <array>
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

        // RFC 7616 3.4: Digest, for this realm, and for this request.
        const bool usable = Util::to_lower(credentials->type) == "digest" && digest::is_complete(credentials) && credentials->contains_field("username") &&
                            credentials->fields["realm"] == realm->name && credentials->fields["uri"] == target;
        if (!usable) return _challenge(realm->name, realm->nonce_expiry, false, then);

        _datastore->nonce_check(_executor, credentials->fields["nonce"], [this, self, method, credentials, realm, then](plugins::Result<bool> checked) {
          if (!checked.ok) return then(Outcome{Outcome::Kind::unavailable, nullptr, {}});

          // A nonce that has expired is stale (RFC 7616 3.3): the client signs again without asking its user.
          if (!checked.value) return _challenge(realm->name, realm->nonce_expiry, true, then);

          auto identity = std::make_shared<types::SIPIdentity>("sip:" + credentials->fields["username"] + "@" + realm->name);
          _datastore->subscriber_get(_executor, identity,
                                     [this, self, method, credentials, realm, then](plugins::Result<std::shared_ptr<types::Subscriber>> found) {
                                       if (!found.ok) return then(Outcome{Outcome::Kind::unavailable, nullptr, {}});

                                       // An unknown subscriber is challenged like a wrong password, so subscribers cannot be enumerated.
                                       if (!found.value) return _challenge(realm->name, realm->nonce_expiry, false, then);

                                       if (const auto why = digest::verify(*found.value, *credentials, method); !why.empty()) {
                                         _logger->info("Subscriber request for " + found.value->identity->to_string() + " with " + why + " - challenging");
                                         return _challenge(realm->name, realm->nonce_expiry, false, then);
                                       }

                                       then(Outcome{Outcome::Kind::ok, found.value, {}});
                                     });
        });
      });
}

void SubscriberAuth::_challenge(const std::string& realm_name, std::uint32_t nonce_expiry, bool stale, std::function<void(Outcome)> then) {
  std::array<unsigned char, 16> bytes{};
  RAND_bytes(bytes.data(), static_cast<int>(bytes.size()));
  const auto nonce = Util::to_hex(bytes.data(), bytes.size());
  const auto expires_at = std::time(nullptr) + static_cast<std::time_t>(nonce_expiry > 0 ? nonce_expiry : 300);

  // The nonce is stored before it is sent, or its own check would fail.
  _datastore->nonce_create(_executor, nonce, expires_at, [realm_name, nonce, stale, then](plugins::Status status) {
    if (!status.ok) return then(Outcome{Outcome::Kind::unavailable, nullptr, {}});

    // RFC 7616 3.3: SHA-256 first, then MD5 for clients that have only that, each with qop=auth.
    Outcome outcome{Outcome::Kind::challenge, nullptr, {}};
    for (const std::string algorithm : {"SHA-256", "MD5"}) {
      outcome.challenges.push_back("Digest realm=\"" + realm_name + "\", qop=\"auth\", algorithm=" + algorithm + ", nonce=\"" + nonce + "\"" +
                                   (stale ? ", stale=true" : ""));
    }
    then(std::move(outcome));
  });
}

}  // namespace athenasip::api
