//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "registrar.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "channel.h"
#include "core.h"
#include "digest.h"
#include "headers/authorization_header.h"
#include "headers/sip_identity_header.h"
#include "headers/string_header.h"
#include "headers/uint_header.h"
#include "loggers/logger_scoped.h"
#include "push/push_parameters.h"
#include "qualifier.h"
#include "util.h"

namespace athenasip {

using athenasip::headers::AuthorizationHeader;
using athenasip::headers::SIPIdentityHeader;
using athenasip::headers::UIntHeader;

namespace {

// RFC 3261 10.2.2: a single Contact of "*" (STAR, 20.10) with Expires 0 removes every
// binding.
bool is_star_contact(const std::shared_ptr<SIPHeader>& header) {
  const auto& contacts = header->headers_map["Contact"];
  if (contacts.size() != 1) return false;

  auto contact = contacts[0]->as<SIPIdentityHeader>();
  if (contact == nullptr || contact->value == nullptr) return false;

  return contact->value->star;
}

// RFC 3261 20.37 and 19.2: whether an option tag is in a Supported header field.
bool supports(const std::shared_ptr<SIPHeader>& header, const std::string& tag) {
  if (!header->contains("Supported")) return false;

  for (const auto& value : header->headers_map["Supported"]) {
    std::stringstream tags(value->to_string());
    std::string one;
    while (std::getline(tags, one, ',')) {
      one.erase(0, one.find_first_not_of(" \t"));
      one.erase(one.find_last_not_of(" \t") + 1);
      if (Util::to_lower(one) == tag) return true;
    }
  }
  return false;
}

// RFC 5626 section 6: a contact asks for outbound when the request supports "outbound" and
// the contact has both +sip.instance and reg-id. Returns the unquoted instance and the
// reg-id; sets malformed when the reg-id is not a positive number.
std::optional<std::pair<std::string, std::uint32_t>> outbound_of(const std::shared_ptr<SIPMessage>& request, const SIPIdentity& contact, bool& malformed) {
  malformed = false;
  if (!supports(request->header, "outbound")) return std::nullopt;

  const auto instance = contact.tags.find("+sip.instance");
  const auto reg_id = contact.tags.find("reg-id");
  if (instance == contact.tags.end() || reg_id == contact.tags.end()) return std::nullopt;

  std::string value = instance->second;
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);

  try {
    const auto id = std::stoul(reg_id->second);
    if (id == 0 || id > 0x7FFFFFFFul) throw std::out_of_range("reg-id");
    return std::make_pair(value, static_cast<std::uint32_t>(id));
  } catch (const std::exception&) {
    malformed = true;
    return std::nullopt;
  }
}

// RFC 5626 section 6: through an edge proxy, outbound needs the first hop to keep the
// flow, which it signals with "ob" on its Path URI.
bool first_hop_supports_outbound(const std::shared_ptr<SIPMessage>& request) {
  if (!request->header->contains("Path")) return true;

  auto first = request->header->headers_map["Path"][0]->to_string();
  const auto end = first.find('>');
  if (end != std::string::npos) first = first.substr(0, end);

  const auto at = first.find(";ob");
  return at != std::string::npos && (at + 3 == first.size() || first[at + 3] == ';' || first[at + 3] == '>');
}

// RFC 8599 5.6.1.1: a Feature-Caps with +sip.pns means a proxy nearer the client will push, so this node does
// not.
bool pushed_by_another_proxy(const std::shared_ptr<SIPMessage>& request) {
  if (!request->header->contains("Feature-Caps")) return false;

  for (const auto& value : request->header->headers_map["Feature-Caps"]) {
    if (value->to_string().find("+sip.pns=") != std::string::npos) return true;
  }
  return false;
}

// RFC 3261 10.3 step 7: an interval of an hour or more is never too brief.
constexpr std::uint32_t kNeverTooBrief = 3600;

// The longest registration granted, and the default requested, when the realm sets no
// maximum.
constexpr std::uint32_t kDefaultMaximumExpiry = 3600;

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
    // A datastore failure is a 500; only a realm that is not served is a 404.
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

  // RFC 3261 10.3 step 5: an unserved domain is a 404. An unknown subscriber in a served
  // realm is challenged instead, so REGISTER cannot enumerate subscribers.
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

  if (!digest::is_complete(auth)) {
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

  if (const auto why = digest::verify(*subscriber, *auth, request->header->request_method); !why.empty()) {
    _logger->info("REGISTER for " + aor->to_string() + " with " + why + " - challenging");
    return _send_challenge(transaction, request, realm);
  }

  request->authenticated = true;

  // The connection now belongs to this subscriber; see Channel::authenticated_as.
  if (auto channel = request->channel.lock()) channel->authenticated_as(aor->uri->to_string());

  _apply_bindings(request, transaction, realm, subscriber);
}

void Registrar::_apply_bindings(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                                std::shared_ptr<types::Realm> realm, std::shared_ptr<types::Subscriber> subscriber) {
  auto core = _core.lock();
  if (!core) return;

  const auto requested = _requested_expiry(request, realm);
  const auto expires = _granted_expiry(requested, realm);

  // A REGISTER with no Contact queries the current bindings (RFC 3261 10.2.2); step 7 does
  // not apply.
  if (!request->header->contains("Contact")) {
    return _send_ok(transaction, request, subscriber, expires);
  }

  auto self = shared_from_this();

  if (is_star_contact(request->header)) {
    if (expires != 0) {
      _logger->info("REGISTER with Contact * and a non-zero expiry - 400");
      return _send_status(transaction, request, 400, "Bad Request");
    }

    // List every binding, then remove them one at a time.
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
  const auto qualify = realm ? realm->behaviour.qualify_over(core->config->behaviour_qualify_interval) : core->config->behaviour_qualify_interval;
  const bool pushed_elsewhere = pushed_by_another_proxy(request);

  for (const auto& header : request->header->headers_map["Contact"]) {
    auto contact = header->as<SIPIdentityHeader>();
    if (contact == nullptr || contact->value == nullptr || contact->value->uri == nullptr) continue;

    // A per-contact expires parameter overrides the Expires header for that one binding.
    auto contact_requested = requested;
    const auto tag = contact->value->tags.find("expires");
    if (tag != contact->value->tags.end()) {
      try {
        contact_requested = static_cast<std::uint32_t>(std::stoul(tag->second));
      } catch (const std::exception&) {
        return _send_status(transaction, request, 400, "Bad Request");
      }
    }

    // Step 7: the first contact that is too brief refuses the whole request.
    if (_is_too_brief(contact_requested, realm)) {
      _logger->info("REGISTER asked for " + std::to_string(contact_requested) + "s, below the realm minimum - 423");
      return _send_interval_too_brief(transaction, request, realm ? realm->registration_minimum : 0);
    }

    Binding binding{contact->value->uri, _granted_expiry(contact_requested, realm), qualify};

    // RFC 8599 5.6.1: a contact with a pn-provider asks for push, or with no pn-prid asks only whether it is
    // supported. This node is the registrar and knows no other proxy pushes, so an unsupported service is a
    // 555 rather than a silent registration without push.
    if (const auto pn = push::notification_of(*contact->value->uri); pn && !pushed_elsewhere && contact_requested != 0) {
      const auto service = core->push_service(pn->provider);

      if (pn->prid.empty()) {
        if (pn->provider.empty() ? core->push_services().empty() : service == nullptr) {
          _logger->info("REGISTER asked whether push is supported for '" + pn->provider + "', and it is not - 555");
          return _send_status(transaction, request, 555, "Push Notification Service Not Supported");
        }
      } else {
        if (service == nullptr || !service->accepts(*pn)) {
          _logger->info("REGISTER asked for " + pn->provider + " push, which is not served or lacks what it needs - 555");
          return _send_status(transaction, request, 555, "Push Notification Service Not Supported");
        }

        // 5.6.1.1: a binding that would expire before its refresh push is too brief.
        const auto minimum = core->config->push_minimum_expiry();
        if (contact_requested < minimum) {
          _logger->info("REGISTER asked for push with " + std::to_string(contact_requested) + "s, too brief to be woken in time - 423");
          return _send_interval_too_brief(transaction, request, std::max(minimum, realm ? realm->registration_minimum : 0u));
        }

        // The realm may grant less than was asked; then the binding registers without push (5.6.1.1).
        binding.push = binding.expires >= minimum;
      }
    }

    bool malformed = false;
    if (const auto outbound = outbound_of(request, *contact->value, malformed)) {
      if (!first_hop_supports_outbound(request)) {
        _logger->info("REGISTER asked for outbound through a first hop that does not support it - 439");
        return _send_status(transaction, request, 439, "First Hop Lacks Outbound Support");
      }
      binding.instance = outbound->first;
      binding.reg_id = outbound->second;
    } else if (malformed) {
      return _send_status(transaction, request, 400, "Bad Request");
    }

    bindings->push_back(std::move(binding));
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

  // RFC 5626 section 6: an outbound binding is keyed by instance and reg-id, not contact.
  // One held under the same pair with a different contact is the same flow reconnected, so
  // it is removed first. A removal also goes by the pair.
  if (binding.reg_id != 0) {
    return core->location_list(subscriber->id, [this, self, request, transaction, subscriber, bindings, index, expires_seconds, binding,
                                                channel](plugins::Result<std::vector<types::Location>> found) {
      auto core = _core.lock();
      if (!core) return;

      std::vector<std::shared_ptr<types::SIPUri>> stale;
      if (found.ok) {
        for (const auto& location : found.value) {
          if (location.instance == binding.instance && location.reg_id == binding.reg_id && location.contact &&
              (binding.expires == 0 || location.contact->to_string() != binding.contact->to_string())) {
            stale.push_back(location.contact);
          }
        }
      }

      _remove_then(subscriber, channel, std::move(stale), 0,
                   [this, self, request, transaction, subscriber, bindings, index, expires_seconds, binding, channel]() {
                     _store_binding(request, transaction, subscriber, bindings, index, expires_seconds, binding, channel);
                   });
    });
  }

  _store_binding(request, transaction, subscriber, bindings, index, expires_seconds, binding, channel);
}

// Removes each contact in turn, then continues whether or not the removals succeeded.
void Registrar::_remove_then(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<Channel> channel,
                             std::vector<std::shared_ptr<types::SIPUri>> contacts, std::size_t index, std::function<void()> then) {
  auto core = _core.lock();
  if (!core) return;
  if (index >= contacts.size()) return then();

  core->qualifier()->forget(subscriber->identity->uri->to_string(), contacts[index]);

  auto self = shared_from_this();
  core->subscriber_unregister(subscriber, contacts[index], channel, [this, self, subscriber, channel, contacts, index, then](plugins::Status) mutable {
    _remove_then(subscriber, channel, std::move(contacts), index + 1, std::move(then));
  });
}

void Registrar::_store_binding(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                               std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<std::vector<Binding>> bindings, std::size_t index,
                               std::uint32_t expires_seconds, Binding binding, std::shared_ptr<Channel> channel) {
  auto core = _core.lock();
  if (!core) return;

  auto self = shared_from_this();

  auto next = [this, self, request, transaction, subscriber, bindings, index, expires_seconds]() {
    _write_bindings(request, transaction, subscriber, bindings, index + 1, expires_seconds);
  };

  if (binding.expires == 0) {
    // A failed removal does not stop the rest.
    core->qualifier()->forget(subscriber->identity->uri->to_string(), binding.contact);
    core->push_refresher()->forget(subscriber->id, binding.contact);
    return core->subscriber_unregister(subscriber, binding.contact, channel, [next](plugins::Status) { next(); });
  }

  core->subscriber_register(
      subscriber, binding.contact, channel, binding.expires, _path_of(request),
      [this, self, request, transaction, subscriber, binding, channel, next](plugins::Status status) {
        if (!status.ok) {
          _logger->error("REGISTER could not store the binding for " + subscriber->identity->to_string() + " - " + status.error);
          return _send_status(transaction, request, 500, "Server Internal Error");
        }

        if (auto core = _core.lock()) {
          if (channel) core->qualifier()->watch(subscriber->identity->uri->to_string(), binding.contact, channel->flow_id(), binding.qualify, binding.expires);

          if (binding.push) {
            core->push_refresher()->watch(subscriber->id, binding.contact, binding.expires);
          } else {
            core->push_refresher()->forget(subscriber->id, binding.contact);
          }

          core->binding_registered(subscriber->id, binding.contact);
        }

        next();
      },
      binding.instance, binding.reg_id, binding.push);
}

std::uint32_t Registrar::_requested_expiry(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<types::Realm>& realm) const {
  if (request->header->contains("Contact")) {
    auto contact = request->header->headers_map["Contact"][0]->as<SIPIdentityHeader>();
    if (contact != nullptr && contact->value != nullptr) {
      const auto tag = contact->value->tags.find("expires");
      if (tag != contact->value->tags.end()) {
        try {
          return static_cast<std::uint32_t>(std::stoul(tag->second));
        } catch (const std::exception&) {
          // Fall through to the Expires header.
        }
      }
    }
  }

  if (request->header->contains("Expires")) {
    auto expires = request->header->headers_map["Expires"][0]->as<UIntHeader>();
    if (expires != nullptr) return static_cast<std::uint32_t>(expires->value);
  }

  // RFC 3261 10.3 step 7: with neither, the local default applies, which is the realm's
  // maximum.
  return realm && realm->registration_timeout > 0 ? realm->registration_timeout : kDefaultMaximumExpiry;
}

std::uint32_t Registrar::_granted_expiry(std::uint32_t requested, const std::shared_ptr<types::Realm>& realm) const {
  const std::uint32_t maximum = realm && realm->registration_timeout > 0 ? realm->registration_timeout : kDefaultMaximumExpiry;
  return std::min(requested, maximum);
}

bool Registrar::_is_too_brief(std::uint32_t requested, const std::shared_ptr<types::Realm>& realm) const {
  if (!realm || realm->registration_minimum == 0) return false;

  // RFC 3261 10.3 step 7: too brief only if greater than zero (zero is a removal), under an
  // hour, and below the realm's minimum.
  return requested > 0 && requested < kNeverTooBrief && requested < realm->registration_minimum;
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

  const auto transport = Util::to_lower(channel->_connection->transport_name());

  auto route = std::make_shared<types::SIPUri>();
  route->valid = true;
  route->scheme = (transport == "tls" || transport == "wss") ? "sips" : "sip";

  // The advertised address, not the local endpoint: a wildcard-bound UDP listener's is
  // 0.0.0.0, which no client can route to.
  const auto advertised = core->advertised_for(*channel);
  route->host = advertised.host;
  route->port = advertised.port;

  // RFC 3261 19.1.1: loose routing, so the next hop keeps the Request-URI.
  route->set_parameter("lr", "");
  if (transport != "udp") route->set_parameter("transport", transport);

  auto identity = std::make_shared<types::SIPIdentity>();
  identity->wrapped = true;
  identity->uri = route;

  return std::make_shared<headers::SIPIdentityHeader>(identity);
}

void Registrar::_send_ok(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                         const std::shared_ptr<types::Subscriber>& subscriber, std::uint32_t expires_seconds) {
  auto core = _core.lock();
  if (!core) return;

  auto self = shared_from_this();

  // RFC 3261 10.3 step 8: list every current binding with its remaining lifetime.
  core->location_list(subscriber->id, [this, self, transaction, request, expires_seconds](plugins::Result<std::vector<types::Location>> found) {
    auto response = request->generate_response();
    response->header->response_code = 200;
    response->header->response_message = "OK";

    const auto now = std::time(nullptr);

    if (!found.ok) {
      // The bindings were written but cannot be read back. A 200 with no Contact would say the
      // registration is gone, so answer 500 and let the client retry.
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

      // RFC 5626 section 6: an outbound binding is listed with what identifies it.
      if (location.reg_id != 0) {
        contact->tags["+sip.instance"] = "\"" + location.instance + "\"";
        contact->tags["reg-id"] = std::to_string(location.reg_id);
      }

      response->header->add("Contact", std::make_shared<SIPIdentityHeader>(contact));
    }

    response->header->add("Expires", std::make_shared<UIntHeader>(expires_seconds));

    // RFC 5626 section 6: Require: outbound when outbound was honoured for a contact. No
    // Flow-Timer is sent: it is optional, and the client's default keep-alive interval is
    // shorter than this node's idle UDP flow timeout.
    bool honoured = false;
    for (const auto& header : request->header->headers_map["Contact"]) {
      auto contact = header->as<SIPIdentityHeader>();
      bool malformed = false;
      if (contact != nullptr && contact->value != nullptr && outbound_of(request, *contact->value, malformed)) honoured = true;
    }
    if (honoured) response->header->add("Require", std::make_shared<headers::StringHeader>("outbound"));

    // RFC 3608: tell the client to route later requests through this node, which holds its
    // flow; the client's own Contact is often unroutable.
    if (auto service_route = _service_route(request)) response->header->add("Service-Route", service_route);

    for (const auto& value : _push_capabilities(request, found.value)) response->header->add("Feature-Caps", std::make_shared<headers::StringHeader>(value));

    transaction->send(response);
  });
}

std::vector<std::string> Registrar::_push_capabilities(const std::shared_ptr<SIPMessage>& request, const std::vector<types::Location>& bindings) const {
  auto core = _core.lock();
  if (!core || pushed_by_another_proxy(request) || !request->header->contains("Contact")) return {};

  // Provider, and whether the client asked to refresh its binding itself (4.1.4).
  std::map<std::string, bool> announced;

  for (const auto& header : request->header->headers_map["Contact"]) {
    auto contact = header->as<SIPIdentityHeader>();
    if (contact == nullptr || contact->value == nullptr || contact->value->uri == nullptr) continue;

    const auto pn = push::notification_of(*contact->value->uri);
    if (!pn) continue;

    // 5.6.1.2: a query is answered for the service named, or for every one when none is.
    if (pn->prid.empty()) {
      for (const auto& [provider, service] : core->push_services()) {
        if (pn->provider.empty() || provider == pn->provider) announced.emplace(provider, false);
      }
      continue;
    }

    // 5.6.1.1: a request for push is answered only if the binding took it.
    const auto uri = contact->value->uri->to_string();
    for (const auto& binding : bindings) {
      if (binding.push && binding.contact && binding.contact->to_string() == uri) {
        announced[pn->provider] = announced[pn->provider] || contact->value->tags.count("+sip.pnsreg") > 0;
      }
    }
  }

  // 5.4: one Feature-Caps per service, its other indicators beside it.
  std::vector<std::string> values;
  for (const auto& [provider, pnsreg] : announced) {
    const auto service = core->push_service(provider);
    if (!service) continue;

    std::string value = "*;+sip.pns=\"" + provider + "\"";
    for (const auto& [name, indicator] : service->capabilities()) value += ";" + name + "=\"" + indicator + "\"";
    if (pnsreg) value += ";+sip.pnsreg=\"" + std::to_string(core->config->push_refresh) + "\"";
    values.push_back(std::move(value));
  }
  return values;
}

void Registrar::_send_status(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request, std::uint16_t code,
                             const std::string& reason) {
  auto response = request->generate_response();
  response->header->response_code = code;
  response->header->response_message = reason;

  transaction->send(response);
}

void Registrar::_send_interval_too_brief(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                                         std::uint32_t minimum) {
  auto response = request->generate_response();
  response->header->response_code = 423;
  response->header->response_message = "Interval Too Brief";

  // RFC 3261 10.3 step 7: a 423 must carry Min-Expires so the client knows what to retry
  // with.
  response->header->add("Min-Expires", std::make_shared<UIntHeader>(minimum));

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

  // The nonce must be stored before the challenge is sent, or its own check would fail.
  core->nonce_create(realm, [this, self, transaction, request, response, realm](plugins::Result<std::string> nonce) {
    if (!nonce.ok) {
      _logger->error("Cannot mint a nonce for " + realm->name + " - " + nonce.error);
      return _send_status(transaction, request, 500, "Server Internal Error");
    }

    digest::add_challenges(*response->header, "WWW-Authenticate", realm->name, nonce.value);

    transaction->send(response);
  });
}

}  // namespace athenasip
