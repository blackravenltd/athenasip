//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "loggers/logger.h"
#include "sip_message.h"
#include "transaction_user.h"
#include "transactions/transaction_base.h"
#include "types/authorization.h"
#include "types/location.h"
#include "types/realm.h"
#include "types/subscriber.h"

namespace athenasip {

class Core;

// RFC 3261 section 10: the registrar, and the transaction user for REGISTER.
//
// Authenticates with Digest against the subscriber's HA1, writes or removes the requested
// bindings, and answers 200 OK listing every current binding. RFC 3327 Path is stored with
// the binding.
class Registrar : public TransactionUser {
 public:
  Registrar(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Core> core);

  void on_request(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction) override;

 private:
  // What the policy granted a REGISTER it accepted: the lifetimes and the qualify interval.
  struct Terms {
    std::uint32_t max_expires = 0;
    std::uint32_t min_expires = 0;
    std::uint32_t qualify_interval = 0;
  };

  // The steps of RFC 3261 10.3, once the policy has accepted the REGISTER into a realm. Each stage resumes after
  // a datastore round trip and is named for what it has just learned.
  void _on_realm(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, std::shared_ptr<types::SIPIdentity> aor,
                 std::shared_ptr<types::Realm> realm, Terms terms);
  void _on_nonce_checked(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                         std::shared_ptr<types::SIPIdentity> aor, std::shared_ptr<types::Realm> realm, std::shared_ptr<types::Authorization> auth, Terms terms);
  void _on_subscriber(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, std::shared_ptr<types::SIPIdentity> aor,
                      std::shared_ptr<types::Realm> realm, std::shared_ptr<types::Authorization> auth, std::shared_ptr<types::Subscriber> subscriber,
                      Terms terms);
  void _apply_bindings(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                       std::shared_ptr<types::Subscriber> subscriber, Terms terms);

  // Bindings are written one at a time; the response waits for the last.
  struct Binding {
    std::shared_ptr<types::SIPUri> contact;
    std::uint32_t expires = 0;

    // Seconds between OPTIONS probes, from the realm's behaviour; zero for none.
    std::uint32_t qualify = 0;

    // RFC 5626 outbound identity, when the client asked for outbound.
    std::string instance;
    std::uint32_t reg_id = 0;

    // RFC 8599: this node will push to the client.
    bool push = false;
  };

  void _write_bindings(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                       std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<std::vector<Binding>> bindings, std::size_t index,
                       std::uint32_t expires_seconds);
  void _store_binding(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                      std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<std::vector<Binding>> bindings, std::size_t index,
                      std::uint32_t expires_seconds, Binding binding, std::shared_ptr<Channel> channel);
  void _remove_then(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<Channel> channel, std::vector<std::shared_ptr<types::SIPUri>> contacts,
                    std::size_t index, std::function<void()> then);

  // The lifetime the client asked for: the Contact's expires parameter, else the Expires
  // header, else the longest granted (RFC 3261 10.3 step 7).
  std::uint32_t _requested_expiry(const std::shared_ptr<SIPMessage>& request, const Terms& terms) const;

  // The lifetime granted: the request capped at the longest the policy grants.
  std::uint32_t _granted_expiry(std::uint32_t requested, const Terms& terms) const;

  // Whether 10.3 step 7 lets the policy's minimum refuse the interval.
  bool _is_too_brief(std::uint32_t requested, const Terms& terms) const;

  // RFC 3327: every Path header, in order, as one field value.
  std::string _path_of(const std::shared_ptr<SIPMessage>& request) const;

  // RFC 3608: this node's URI on the flow the REGISTER arrived over, for the client to route
  // later requests through. Null when there is no flow.
  std::shared_ptr<headers::Header> _service_route(const std::shared_ptr<SIPMessage>& request) const;

  // 200 OK with a Contact for every live binding and its remaining lifetime, plus Expires
  // (RFC 3261 10.3 step 8).
  void _send_ok(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                const std::shared_ptr<types::Subscriber>& subscriber, std::uint32_t expires_seconds);

  void _send_status(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request, std::uint16_t code,
                    const std::string& reason);

  // 423 with the required Min-Expires (RFC 3261 10.3 step 7, 10.2.8).
  void _send_interval_too_brief(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                                std::uint32_t minimum);

  // RFC 8599 5.4: the Feature-Caps values for a 2xx to REGISTER, one per push service the client asked about
  // or was granted.
  std::vector<std::string> _push_capabilities(const std::shared_ptr<SIPMessage>& request, const std::vector<types::Location>& bindings) const;

  // AthenaSIP-Alternate-Server: the other nodes to register with if this one goes, for a client that asked with
  // Supported: athenasip-failover over TLS or WSS. Contact grammar, each with how long the list holds.
  std::vector<std::string> _alternate_servers(const std::shared_ptr<SIPMessage>& request, std::uint32_t expires_seconds) const;

  // 401 with a fresh nonce for the realm, if the realm is known.
  void _send_challenge(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                       const std::shared_ptr<types::Realm>& realm);

  std::shared_ptr<loggers::Logger> _logger;
  std::weak_ptr<Core> _core;
};

}  // namespace athenasip
