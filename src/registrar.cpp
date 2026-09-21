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

#include "channel.h"
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

// RFC 3261 10.2.2: a single Contact of "*" with Expires 0 removes every binding. The
// grammar has STAR as its own alternative (20.10), so the identity says whether it saw
// one rather than this having to recognise the shape a "*" left behind.
bool is_star_contact(const std::shared_ptr<SIPHeader>& header) {
  const auto& contacts = header->headers_map["Contact"];
  if (contacts.size() != 1) return false;

  auto contact = contacts[0]->as<SIPIdentityHeader>();
  if (contact == nullptr || contact->value == nullptr) return false;

  return contact->value->star;
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
  core->realm_get_by_name(Util::to_lower(aor->uri->host), [this, self, request, transaction, aor](plugins::Result<std::shared_ptr<types::Realm>> found) {
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
  // An account that does not exist inside a realm we do serve is challenged instead,
  // so a REGISTER sweep cannot enumerate accounts.
  if (!realm) {
    _logger->info("REGISTER for unserved domain " + aor->uri->host + " - 404");
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
  core->account_get(aor, [this, self, request, transaction, aor, realm, auth](plugins::Result<std::shared_ptr<types::Account>> found) {
    if (!found.ok) {
      _logger->error("REGISTER could not read the account - " + found.error);
      return _send_status(transaction, request, 500, "Server Internal Error");
    }

    _on_account(request, transaction, aor, realm, auth, found.value);
  });
}

void Registrar::_on_account(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                            std::shared_ptr<types::SIPIdentity> aor, std::shared_ptr<types::Realm> realm, std::shared_ptr<types::Authorization> auth,
                            std::shared_ptr<types::Account> account) {
  if (!account) {
    _logger->info("REGISTER for unknown account " + aor->to_string() + " - challenging");
    return _send_challenge(transaction, request, realm);
  }

  // RFC 8760: the client answered one of the challenges, and which one it answered says
  // which hash to check with. Absent means MD5, which is what RFC 2617 3.2.1 says and
  // what every client that has never heard of anything else sends.
  const auto algorithm = Util::to_upper(auth->contains_field("algorithm") ? auth->fields["algorithm"] : "MD5");

  if (algorithm != "MD5" && algorithm != "SHA-256") {
    _logger->info("REGISTER with an unsupported Digest algorithm " + algorithm + " - challenging");
    return _send_challenge(transaction, request, realm);
  }

  const auto& stored = algorithm == "SHA-256" ? account->ha1_sha256 : account->ha1;

  // An account with no credential for the algorithm it answered with cannot be checked.
  // Challenging again is the honest answer: the next challenge carries both algorithms
  // and the client can come back with the other one.
  if (stored.empty()) {
    _logger->info("REGISTER answered with " + algorithm + " but " + aor->to_string() + " has no credential for it - challenging");
    return _send_challenge(transaction, request, realm);
  }

  // RFC 2617: HA1 is stored, so the check is HA1:nonce:HA2 with HA2 over method and URI.
  const auto hash = [&algorithm](const std::string& input) { return algorithm == "SHA-256" ? Util::sha256(input) : Util::md5(input); };

  const auto expected = Util::to_lower(hash(stored + ":" + auth->fields["nonce"] + ":" + hash(request->header->request_method + ":" + auth->fields["uri"])));

  if (expected != Util::to_lower(auth->fields["response"])) {
    _logger->info("REGISTER Digest response mismatch for " + aor->to_string() + " - challenging");
    return _send_challenge(transaction, request, realm);
  }

  request->authenticated = true;

  _apply_bindings(request, transaction, realm, account);
}

void Registrar::_apply_bindings(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                                std::shared_ptr<types::Realm> realm, std::shared_ptr<types::Account> account) {
  auto core = _core.lock();
  if (!core) return;

  const auto expires = _requested_expiry(request, realm);

  // A REGISTER with no Contact is a query for the current bindings (RFC 3261 10.2.2).
  if (!request->header->contains("Contact")) {
    return _send_ok(transaction, request, account, expires);
  }

  auto self = shared_from_this();

  if (is_star_contact(request->header)) {
    if (expires != 0) {
      _logger->info("REGISTER with Contact * and a non-zero expiry - 400");
      return _send_status(transaction, request, 400, "Bad Request");
    }

    // Every binding goes, which means reading them first and then removing them one at
    // a time: each removal is its own round trip.
    return core->location_list(account->id, [this, self, request, transaction, account](plugins::Result<std::vector<types::Location>> found) {
      if (!found.ok) {
        _logger->error("REGISTER could not list the bindings - " + found.error);
        return _send_status(transaction, request, 500, "Server Internal Error");
      }

      auto bindings = std::make_shared<std::vector<Binding>>();
      for (const auto& location : found.value) {
        if (location.contact) bindings->push_back(Binding{location.contact, 0});
      }

      _write_bindings(request, transaction, account, bindings, 0, 0);
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

  _write_bindings(request, transaction, account, bindings, 0, expires);
}

void Registrar::_write_bindings(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                                std::shared_ptr<types::Account> account, std::shared_ptr<std::vector<Binding>> bindings, std::size_t index,
                                std::uint32_t expires_seconds) {
  auto core = _core.lock();
  if (!core) return;

  if (index >= bindings->size()) {
    return _send_ok(transaction, request, account, expires_seconds);
  }

  const auto binding = (*bindings)[index];
  auto channel = request->channel.lock();
  auto self = shared_from_this();

  auto next = [this, self, request, transaction, account, bindings, index, expires_seconds]() {
    _write_bindings(request, transaction, account, bindings, index + 1, expires_seconds);
  };

  if (binding.expires == 0) {
    // A removal that fails is logged and the rest still go: the client asked for all of
    // them and a partial answer is better than none.
    return core->account_unregister(account, binding.contact, channel, [next](plugins::Status) { next(); });
  }

  core->account_register(account, binding.contact, channel, binding.expires, _path_of(request),
                         [this, self, request, transaction, account, next](plugins::Status status) {
                           if (!status.ok) {
                             _logger->error("REGISTER could not store the binding for " + account->identity->to_string() + " - " + status.error);
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

std::shared_ptr<headers::Header> Registrar::_service_route(const std::shared_ptr<SIPMessage>& request) const {
  auto core = _core.lock();
  auto channel = request->channel.lock();
  if (!core || !channel || !channel->_connection) return nullptr;

  const auto local = channel->_connection->local_endpoint();
  const auto transport = Util::to_lower(channel->_connection->transport_name());

  auto route = std::make_shared<types::SIPUri>();
  route->valid = true;
  route->scheme = (transport == "tls" || transport == "wss") ? "sips" : "sip";

  // The address the node was told to advertise, when it was told one. A UDP listener is
  // bound to the wildcard, so its local endpoint is 0.0.0.0 - and a Service-Route
  // pointing at 0.0.0.0 is a route the client cannot use, which is worse than sending
  // none at all.
  route->host = core->config->sip_public_address.empty() ? local.address().to_string() : core->config->sip_public_address;
  route->port = local.port();

  // 19.1.1 again: loose routing, so the next hop does not rewrite the Request-URI.
  route->set_parameter("lr", "");
  if (transport != "udp") route->set_parameter("transport", transport);

  auto identity = std::make_shared<types::SIPIdentity>();
  identity->wrapped = true;
  identity->uri = route;

  return std::make_shared<headers::SIPIdentityHeader>(identity);
}

void Registrar::_send_ok(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                         const std::shared_ptr<types::Account>& account, std::uint32_t expires_seconds) {
  auto core = _core.lock();
  if (!core) return;

  auto self = shared_from_this();

  // RFC 3261 10.3 step 8: list every binding that is now current, each with the time it
  // has left, so a client that lost track can resynchronise from the response alone.
  // Reading them is a round trip, so the response is built in the handler.
  core->location_list(account->id, [this, self, transaction, request, expires_seconds](plugins::Result<std::vector<types::Location>> found) {
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

    // RFC 3608: where everything after this registration should go. The client reached
    // this node over a flow this node holds, and its own Contact is often unroutable -
    // a browser's always is - so telling it to route back through here is what makes
    // the next request work at all. It is also half of failover: a client that
    // re-registers somewhere else is told that node's route by that node, and needs no
    // DNS to learn it.
    if (auto service_route = _service_route(request)) response->header->add("Service-Route", service_route);

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

    // RFC 8760 section 2.1: one challenge per algorithm, most preferred first, and the
    // client answers the first one it supports. SHA-256 leads because a client that can
    // do better than MD5 should, and MD5 follows because nearly every SIP client can do
    // nothing else (RFC 3261 22.4 knows only MD5).
    //
    // A challenge is sent before this node knows which account is answering, so both go
    // out every time. An account with no SHA-256 credential - one imported as a bare MD5
    // hash - is re-challenged for MD5 alone when it answers with SHA-256.
    for (const auto& algorithm : {std::string("SHA-256"), std::string("MD5")}) {
      types::Authorization challenge;
      challenge.type = "Digest";
      challenge.fields["realm"] = realm->name;
      challenge.fields["nonce"] = nonce.value;
      challenge.fields["algorithm"] = algorithm;

      response->header->add("WWW-Authenticate", challenge.to_string());
    }

    transaction->send(response);
  });
}

}  // namespace athenasip
