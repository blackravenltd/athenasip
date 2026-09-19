//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "registrar.h"

#include <algorithm>
#include <ctime>
#include <utility>

#include "core.h"
#include "headers/authorization_header.h"
#include "headers/sip_identity_header.h"
#include "headers/uint_header.h"
#include "loggers/logger_scoped.h"
#include "util.h"

namespace athenasip {

using athenasip::headers::AuthorizationHeader;
using athenasip::headers::SIPIdentityHeader;
using athenasip::headers::UIntHeader;

namespace {

// RFC 3261 10.2.2: a single Contact of "*" with Expires 0 removes every binding.
//
// "*" is not a URI, and the identity parser has nowhere to put it: it lands in the host
// part with no user and the URI marked invalid. Recognising that shape is the best that
// can be done until the SIPUri rewrite gives Contact somewhere honest to keep it.
bool is_star_contact(const std::shared_ptr<SIPHeader>& header) {
  const auto& contacts = header->headers_map["Contact"];
  if (contacts.size() != 1) return false;

  auto contact = contacts[0]->as<SIPIdentityHeader>();
  if (contact == nullptr || contact->value == nullptr || contact->value->uri == nullptr) return false;

  return contact->value->uri->user.empty() && contact->value->uri->realm == "*";
}

}  // namespace

Registrar::Registrar(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Core> core)
    : _logger(std::make_shared<loggers::LoggerScoped>("registrar", std::move(logger))), _core(core) {}

void Registrar::on_request(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction) {
  auto core = _core.lock();
  if (!core || !transaction) return;

  // The address of record is the To (RFC 3261 10.2), not the From: a third party may
  // register on someone else's behalf.
  if (!request->header->contains("To")) {
    _logger->info("REGISTER with no To - 400");
    return _send_status(transaction, request, 400, "Bad Request");
  }

  auto to = request->header->headers_map["To"][0]->as<SIPIdentityHeader>();
  if (to == nullptr || to->value == nullptr || to->value->uri == nullptr) {
    return _send_status(transaction, request, 400, "Bad Request");
  }

  auto aor = to->value;

  auto self = shared_from_this();
  core->realm_get_by_name(Util::to_lower(aor->uri->realm), [this, self, request, transaction, aor](plugins::Result<std::shared_ptr<types::Realm>> found) {
    // A datastore that cannot answer is not a domain we do not
    // serve. One is 500, the other 404, and before the contract
    // could report the difference both looked like "no realm".
    if (!found.ok) {
      _logger->error("REGISTER could not read the realm - " + found.error);
      return _send_status(transaction, request, 500, "Server Internal Error");
    }

    _on_realm(request, transaction, aor, found.value);
  });
}

void Registrar::_on_realm(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                          std::shared_ptr<types::SIPIdentity> aor, std::shared_ptr<types::Realm> realm) {
  auto core = _core.lock();
  if (!core) return;

  // RFC 3261 10.3 step 5: an address of record this registrar does not serve is a 404.
  // A subscriber that does not exist inside a realm we do serve is challenged instead,
  // so a REGISTER sweep cannot enumerate accounts.
  if (!realm) {
    _logger->info("REGISTER for unserved domain " + aor->uri->realm + " - 404");
    return _send_status(transaction, request, 404, "Not Found");
  }

  if (!request->header->contains("Authorization")) {
    _logger->info("REGISTER with no Authorization - challenging");
    return _send_challenge(transaction, request, realm);
  }

  auto auth_header = request->header->headers_map["Authorization"][0]->as<AuthorizationHeader>();
  auto auth = auth_header != nullptr ? auth_header->value : nullptr;

  if (!auth || auth->type != "Digest" || !auth->contains_field("realm") || !auth->contains_field("nonce") || !auth->contains_field("response") ||
      !auth->contains_field("uri")) {
    _logger->info("REGISTER with incomplete Digest credentials - challenging");
    return _send_challenge(transaction, request, realm);
  }

  auto self = shared_from_this();
  core->nonce_check(auth->fields["nonce"], [this, self, request, transaction, aor, realm, auth](plugins::Result<bool> checked) {
    if (!checked.ok) {
      _logger->error("REGISTER could not check the nonce - " + checked.error);
      return _send_status(transaction, request, 500, "Server Internal Error");
    }

    if (!checked.value) {
      _logger->info("REGISTER nonce " + auth->fields["nonce"] + " not found or expired - challenging");
      return _send_challenge(transaction, request, realm);
    }

    _on_nonce_checked(request, transaction, aor, realm, auth);
  });
}

void Registrar::_on_nonce_checked(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                                  std::shared_ptr<types::SIPIdentity> aor, std::shared_ptr<types::Realm> realm, std::shared_ptr<types::Authorization> auth) {
  auto core = _core.lock();
  if (!core) return;

  auto self = shared_from_this();
  core->subscriber_get(aor, [this, self, request, transaction, aor, realm, auth](plugins::Result<std::shared_ptr<types::Subscriber>> found) {
    if (!found.ok) {
      _logger->error("REGISTER could not read the subscriber - " + found.error);
      return _send_status(transaction, request, 500, "Server Internal Error");
    }

    _on_subscriber(request, transaction, aor, realm, auth, found.value);
  });
}

void Registrar::_on_subscriber(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                               std::shared_ptr<types::SIPIdentity> aor, std::shared_ptr<types::Realm> realm, std::shared_ptr<types::Authorization> auth,
                               std::shared_ptr<types::Subscriber> subscriber) {
  if (!subscriber) {
    _logger->info("REGISTER for unknown subscriber " + aor->to_string() + " - challenging");
    return _send_challenge(transaction, request, realm);
  }

  // RFC 2617: HA1 is stored, so the check is HA1:nonce:HA2 with HA2 over method and URI.
  const auto expected =
      Util::to_lower(Util::md5(subscriber->ha1 + ":" + auth->fields["nonce"] + ":" + Util::md5(request->header->request_method + ":" + auth->fields["uri"])));

  if (expected != Util::to_lower(auth->fields["response"])) {
    _logger->info("REGISTER Digest response mismatch for " + aor->to_string() + " - challenging");
    return _send_challenge(transaction, request, realm);
  }

  request->authenticated = true;

  _apply_bindings(request, transaction, realm, subscriber);
}

void Registrar::_apply_bindings(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                                std::shared_ptr<types::Realm> realm, std::shared_ptr<types::Subscriber> subscriber) {
  auto core = _core.lock();
  if (!core) return;

  const auto expires = _requested_expiry(request, realm);

  // A REGISTER with no Contact is a query for the current bindings (RFC 3261 10.2.2).
  if (!request->header->contains("Contact")) {
    return _send_ok(transaction, request, subscriber, expires);
  }

  auto self = shared_from_this();

  if (is_star_contact(request->header)) {
    if (expires != 0) {
      _logger->info("REGISTER with Contact * and a non-zero expiry - 400");
      return _send_status(transaction, request, 400, "Bad Request");
    }

    // Every binding goes, which means reading them first and then removing them one at
    // a time: each removal is its own round trip.
    return core->location_list(subscriber->id, [this, self, request, transaction, subscriber](plugins::Result<std::vector<types::Location>> found) {
      if (!found.ok) {
        _logger->error("REGISTER could not list the bindings - " + found.error);
        return _send_status(transaction, request, 500, "Server Internal Error");
      }

      auto bindings = std::make_shared<std::vector<Binding>>();
      for (const auto& location : found.value) {
        if (location.contact) bindings->push_back(Binding{location.contact, 0});
      }

      _write_bindings(request, transaction, subscriber, bindings, 0, 0);
    });
  }

  auto bindings = std::make_shared<std::vector<Binding>>();

  for (const auto& header : request->header->headers_map["Contact"]) {
    auto contact = header->as<SIPIdentityHeader>();
    if (contact == nullptr || contact->value == nullptr || contact->value->uri == nullptr) continue;

    // A per-contact expires parameter overrides the Expires header for that one binding.
    auto contact_expires = expires;
    const auto tag = contact->value->tags.find("expires");
    if (tag != contact->value->tags.end()) {
      try {
        contact_expires = static_cast<std::uint32_t>(std::stoul(tag->second));
      } catch (const std::exception&) {
        return _send_status(transaction, request, 400, "Bad Request");
      }

      if (realm->registration_timeout > 0) contact_expires = std::min(contact_expires, realm->registration_timeout);
    }

    bindings->push_back(Binding{contact->value->uri, contact_expires});
  }

  _write_bindings(request, transaction, subscriber, bindings, 0, expires);
}

void Registrar::_write_bindings(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                                std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<std::vector<Binding>> bindings, std::size_t index,
                                std::uint32_t expires_seconds) {
  auto core = _core.lock();
  if (!core) return;

  if (index >= bindings->size()) {
    return _send_ok(transaction, request, subscriber, expires_seconds);
  }

  const auto binding = (*bindings)[index];
  auto channel = request->channel.lock();
  auto self = shared_from_this();

  auto next = [this, self, request, transaction, subscriber, bindings, index, expires_seconds]() {
    _write_bindings(request, transaction, subscriber, bindings, index + 1, expires_seconds);
  };

  if (binding.expires == 0) {
    // A removal that fails is logged and the rest still go: the client asked for all of
    // them and a partial answer is better than none.
    return core->subscriber_unregister(subscriber, binding.contact, channel, [next](plugins::Status) { next(); });
  }

  core->subscriber_register(subscriber, binding.contact, channel, binding.expires, _path_of(request),
                            [this, self, request, transaction, subscriber, next](plugins::Status status) {
                              if (!status.ok) {
                                _logger->error("REGISTER could not store the binding for " + subscriber->identity->to_string() + " - " + status.error);
                                return _send_status(transaction, request, 500, "Server Internal Error");
                              }

                              next();
                            });
}

std::uint32_t Registrar::_requested_expiry(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<types::Realm>& realm) const {
  const std::uint32_t maximum = realm && realm->registration_timeout > 0 ? realm->registration_timeout : 3600;

  if (request->header->contains("Contact")) {
    auto contact = request->header->headers_map["Contact"][0]->as<SIPIdentityHeader>();
    if (contact != nullptr && contact->value != nullptr) {
      const auto tag = contact->value->tags.find("expires");
      if (tag != contact->value->tags.end()) {
        try {
          return std::min(static_cast<std::uint32_t>(std::stoul(tag->second)), maximum);
        } catch (const std::exception&) {
          // Fall through to the Expires header.
        }
      }
    }
  }

  if (request->header->contains("Expires")) {
    auto expires = request->header->headers_map["Expires"][0]->as<UIntHeader>();
    if (expires != nullptr) return std::min(static_cast<std::uint32_t>(expires->value), maximum);
  }

  return maximum;
}

std::string Registrar::_path_of(const std::shared_ptr<SIPMessage>& request) const {
  if (!request->header->contains("Path")) return "";

  std::string path;
  for (const auto& header : request->header->headers_map["Path"]) {
    if (!path.empty()) path += ", ";
    path += header->to_string();
  }

  return path;
}

void Registrar::_send_ok(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                         const std::shared_ptr<types::Subscriber>& subscriber, std::uint32_t expires_seconds) {
  auto core = _core.lock();
  if (!core) return;

  auto self = shared_from_this();

  // RFC 3261 10.3 step 8: list every binding that is now current, each with the time it
  // has left, so a client that lost track can resynchronise from the response alone.
  // Reading them is a round trip, so the response is built in the handler.
  core->location_list(subscriber->id, [this, self, transaction, request, expires_seconds](plugins::Result<std::vector<types::Location>> found) {
    auto response = request->generate_response();
    response->header->response_code = 200;
    response->header->response_message = "OK";

    const auto now = std::time(nullptr);

    if (!found.ok) {
      // The bindings were written; only the read-back failed. Answering 200 with no
      // Contact would tell the client its registration is gone, which is worse than
      // answering 500 and having it retry.
      _logger->error("REGISTER could not list the bindings for the response - " + found.error);
      return _send_status(transaction, request, 500, "Server Internal Error");
    }

    for (const auto& location : found.value) {
      if (!location.contact) continue;

      auto contact = std::make_shared<SIPIdentity>();
      contact->uri = location.contact;
      contact->wrapped = true;

      const auto remaining = location.expires_at > now ? static_cast<std::uint32_t>(location.expires_at - now) : 0u;
      contact->tags["expires"] = std::to_string(remaining);

      response->header->add("Contact", std::make_shared<SIPIdentityHeader>(contact));
    }

    response->header->add("Expires", std::make_shared<UIntHeader>(expires_seconds));

    transaction->send(response);
  });
}

void Registrar::_send_status(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request, std::uint16_t code,
                             const std::string& reason) {
  auto response = request->generate_response();
  response->header->response_code = code;
  response->header->response_message = reason;

  transaction->send(response);
}

void Registrar::_send_challenge(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                                const std::shared_ptr<types::Realm>& realm) {
  auto core = _core.lock();
  if (!core) return;

  auto response = request->generate_response();
  response->header->response_code = 401;
  response->header->response_message = "Unauthorized";
  response->header->add("Reason", "SIP;cause=401;text=\"Unauthorized\"");

  if (!realm) {
    transaction->send(response);
    return;
  }

  auto self = shared_from_this();

  // Minting a nonce writes it to the datastore, so the challenge cannot be built until
  // that lands: a nonce the store never accepted would fail its own check next time.
  core->nonce_create(realm, [this, self, transaction, request, response, realm](plugins::Result<std::string> nonce) {
    if (!nonce.ok) {
      _logger->error("Cannot mint a nonce for " + realm->name + " - " + nonce.error);
      return _send_status(transaction, request, 500, "Server Internal Error");
    }

    // No algorithm parameter: RFC 3261 25.1 makes it a token and the serialiser quotes
    // every field it holds, so sending one would be malformed. MD5 is the default when
    // it is absent (RFC 2617 3.2.1). RFC 8760 SHA-256 arrives with it.
    response->header->add("WWW-Authenticate", "Digest realm=\"" + realm->name + "\", nonce=\"" + nonce.value + "\"");

    transaction->send(response);
  });
}

}  // namespace athenasip
