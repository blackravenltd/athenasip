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
#include "types/realm.h"
#include "types/subscriber.h"

namespace athenasip {

class Core;

// RFC 3261 section 10: the registrar, and the transaction user for REGISTER.
//
// It authenticates with Digest against the subscriber's HA1, writes or removes the
// bindings the request asks for, and answers 200 OK listing every binding it now holds
// with the expiry it granted. RFC 3327 Path is recorded on the binding so a later hop
// knows how to reach the contact.
class Registrar : public TransactionUser {
 public:
  Registrar(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Core> core);

  void on_request(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction) override;

 private:
  // RFC 3261 10.3 is a numbered list of steps, and every step that needs the datastore
  // now returns before the next one runs. Each stage below is one of those resumptions,
  // named for what it has just learned rather than for what it is about to do.
  void _on_realm(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, std::shared_ptr<types::SIPIdentity> aor,
                 std::shared_ptr<types::Realm> realm);
  void _on_nonce_checked(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                         std::shared_ptr<types::SIPIdentity> aor, std::shared_ptr<types::Realm> realm, std::shared_ptr<types::Authorization> auth);
  void _on_subscriber(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, std::shared_ptr<types::SIPIdentity> aor,
                      std::shared_ptr<types::Realm> realm, std::shared_ptr<types::Authorization> auth, std::shared_ptr<types::Subscriber> subscriber);
  void _apply_bindings(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, std::shared_ptr<types::Realm> realm,
                       std::shared_ptr<types::Subscriber> subscriber);

  // One binding at a time, because each write is a round trip and the response cannot
  // be sent until the last of them has landed.
  struct Binding {
    std::shared_ptr<types::SIPUri> contact;
    std::uint32_t expires = 0;

    // Seconds between OPTIONS to it, from the realm's behaviour; zero for none.
    std::uint32_t qualify = 0;

    // RFC 5626 outbound's identity for it, when the client asked for outbound.
    std::string instance;
    std::uint32_t reg_id = 0;
  };

  void _write_bindings(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                       std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<std::vector<Binding>> bindings, std::size_t index,
                       std::uint32_t expires_seconds);
  void _store_binding(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                      std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<std::vector<Binding>> bindings, std::size_t index,
                      std::uint32_t expires_seconds, Binding binding, std::shared_ptr<Channel> channel);
  void _remove_then(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<Channel> channel, std::vector<std::shared_ptr<types::SIPUri>> contacts,
                    std::size_t index, std::function<void()> then);

  // The lifetime the client asked for, from the Contact's expires parameter, then the
  // Expires header, then the realm default (RFC 3261 10.3 step 7). Absent everywhere
  // means the realm default. This is what the client asked for, not what it gets.
  std::uint32_t _requested_expiry(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<types::Realm>& realm) const;

  // The lifetime this realm will actually grant for that request: no longer than its
  // registration_timeout.
  std::uint32_t _granted_expiry(std::uint32_t requested, const std::shared_ptr<types::Realm>& realm) const;

  // Whether 10.3 step 7 allows this realm to refuse the interval outright.
  bool _is_too_brief(std::uint32_t requested, const std::shared_ptr<types::Realm>& realm) const;

  // RFC 3327: every Path header, in order, as one field value.
  std::string _path_of(const std::shared_ptr<SIPMessage>& request) const;

  // RFC 3608: this node, on the flow the REGISTER arrived over, for the client to route
  // everything that follows through. Null when there is no flow to name.
  std::shared_ptr<headers::Header> _service_route(const std::shared_ptr<SIPMessage>& request) const;

  // 200 OK carrying a Contact for every live binding with its remaining lifetime, and an
  // Expires header (RFC 3261 10.3 step 8).
  void _send_ok(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                const std::shared_ptr<types::Subscriber>& subscriber, std::uint32_t expires_seconds);

  void _send_status(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request, std::uint16_t code,
                    const std::string& reason);

  // 423 with the Min-Expires the RFC requires on it, so the client knows what to ask
  // for rather than having to guess its way up (RFC 3261 10.3 step 7, 10.2.8).
  void _send_interval_too_brief(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                                const std::shared_ptr<types::Realm>& realm);

  // 401 with a fresh nonce for the realm, when there is one we recognise.
  void _send_challenge(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                       const std::shared_ptr<types::Realm>& realm);

  std::shared_ptr<loggers::Logger> _logger;
  std::weak_ptr<Core> _core;
};

}  // namespace athenasip
