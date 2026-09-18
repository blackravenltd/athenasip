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
  auto realm = core->realm_get_by_name(Util::to_lower(aor->uri->realm));

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

  if (!core->nonce_check(auth->fields["nonce"])) {
    _logger->info("REGISTER nonce " + auth->fields["nonce"] + " not found or expired - challenging");
    return _send_challenge(transaction, request, realm);
  }

  auto subscriber = core->subscriber_get(aor);
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

  const auto expires = _requested_expiry(request, realm);
  const auto path = _path_of(request);
  auto channel = request->channel.lock();

  // A REGISTER with no Contact is a query for the current bindings (RFC 3261 10.2.2).
  if (!request->header->contains("Contact")) {
    return _send_ok(transaction, request, subscriber, expires);
  }

  if (is_star_contact(request->header)) {
    if (expires != 0) {
      _logger->info("REGISTER with Contact * and a non-zero expiry - 400");
      return _send_status(transaction, request, 400, "Bad Request");
    }

    for (const auto& location : core->datastore->location_list(subscriber->id)) {
      core->subscriber_unregister(subscriber, location.contact, channel);
    }

    return _send_ok(transaction, request, subscriber, 0);
  }

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

    if (contact_expires == 0) {
      core->subscriber_unregister(subscriber, contact->value->uri, channel);
      continue;
    }

    if (!core->subscriber_register(subscriber, contact->value->uri, channel, contact_expires, path)) {
      _logger->error("REGISTER could not store the binding for " + aor->to_string());
      return _send_status(transaction, request, 500, "Server Internal Error");
    }
  }

  _send_ok(transaction, request, subscriber, expires);
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

  auto response = request->generate_response();
  response->header->response_code = 200;
  response->header->response_message = "OK";

  // RFC 3261 10.3 step 8: list every binding that is now current, each with the time it
  // has left, so a client that lost track can resynchronise from the response alone.
  const auto now = std::time(nullptr);

  for (const auto& location : core->datastore->location_list(subscriber->id)) {
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

  if (realm) {
    // No algorithm parameter: RFC 3261 25.1 makes it a token and the serialiser quotes
    // every field it holds, so sending one would be malformed. MD5 is the default when
    // it is absent (RFC 2617 3.2.1). RFC 8760 SHA-256 arrives with it.
    response->header->add("WWW-Authenticate", "Digest realm=\"" + realm->name + "\", nonce=\"" + core->nonce_create(realm) + "\"");
  }

  transaction->send(response);
}

}  // namespace athenasip
