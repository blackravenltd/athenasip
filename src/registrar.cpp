//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "registrar.h"

#include <algorithm>
#include <ctime>
#include <optional>
#include <sstream>
#include <utility>

#include "channel.h"
#include "core.h"
#include "digest.h"
#include "headers/authorization_header.h"
#include "headers/sip_identity_header.h"
#include "headers/string_header.h"
#include "headers/uint_header.h"
#include "loggers/logger_scoped.h"
#include "qualifier.h"
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

// RFC 5626 section 6: a client has asked for outbound on a contact when it says it supports
// outbound and the contact carries both an instance and a reg-id. Anything less is an
// ordinary binding. The instance is held without the quotes the parameter carries it in.
// Zero for the reg-id means not outbound; a reg-id that is not a number is the caller's
// to refuse.
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

// RFC 5626 section 6: through an edge proxy, outbound only works when that first hop keeps
// the flow, which it says with "ob" on the URI it put in Path.
bool first_hop_supports_outbound(const std::shared_ptr<SIPMessage>& request) {
  if (!request->header->contains("Path")) return true;

  auto first = request->header->headers_map["Path"][0]->to_string();
  const auto end = first.find('>');
  if (end != std::string::npos) first = first.substr(0, end);

  const auto at = first.find(";ob");
  return at != std::string::npos && (at + 3 == first.size() || first[at + 3] == ';' || first[at + 3] == '>');
}

// RFC 3261 10.3 step 7 draws the line itself: an interval of an hour or more is never
// too brief, whatever minimum a realm is configured with.
constexpr std::uint32_t kNeverTooBrief = 3600;

// The longest registration granted, and the default asked for, when the realm names no
// maximum of its own.
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

  if (const auto why = digest::verify(*account, *auth, request->header->request_method); !why.empty()) {
    _logger->info("REGISTER for " + aor->to_string() + " with " + why + " - challenging");
    return _send_challenge(transaction, request, realm);
  }

  request->authenticated = true;

  // The connection this arrived on now belongs to the subscriber who proved it. The proxy
  // takes a request as that subscriber on it without a second challenge, if the
  // connection is one a source address cannot be forged onto; see Channel::authenticated_as.
  if (auto channel = request->channel.lock()) channel->authenticated_as(aor->uri->to_string());

  _apply_bindings(request, transaction, realm, account);
}

void Registrar::_apply_bindings(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                                std::shared_ptr<types::Realm> realm, std::shared_ptr<types::Account> account) {
  auto core = _core.lock();
  if (!core) return;

  const auto requested = _requested_expiry(request, realm);
  const auto expires = _granted_expiry(requested, realm);

  // A REGISTER with no Contact is a query for the current bindings (RFC 3261 10.2.2).
  // Step 6 skips to the last step for one of those, so step 7 never runs and there is
  // no interval to find too brief.
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
  const auto qualify = realm ? realm->behaviour.qualify_over(core->config->behaviour_qualify_interval) : core->config->behaviour_qualify_interval;

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

    // Step 7 refuses the whole request on the first contact it will not honour, rather
    // than registering some of them: "It then skips the remaining steps."
    if (_is_too_brief(contact_requested, realm)) {
      _logger->info("REGISTER asked for " + std::to_string(contact_requested) + "s, below the realm minimum - 423");
      return _send_interval_too_brief(transaction, request, realm);
    }

    Binding binding{contact->value->uri, _granted_expiry(contact_requested, realm), qualify};

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

  // RFC 5626 section 6: an outbound binding is its instance and reg-id, not its contact. One
  // already held under the same pair with another contact is the same flow from before -
  // a reconnect, a new port - and goes first, so the write below replaces it rather than
  // leaving a dead binding beside it. A removal goes by the pair as well.
  if (binding.reg_id != 0) {
    return core->location_list(account->id, [this, self, request, transaction, account, bindings, index, expires_seconds, binding,
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

      _remove_then(account, channel, std::move(stale), 0, [this, self, request, transaction, account, bindings, index, expires_seconds, binding, channel]() {
        _store_binding(request, transaction, account, bindings, index, expires_seconds, binding, channel);
      });
    });
  }

  _store_binding(request, transaction, account, bindings, index, expires_seconds, binding, channel);
}

// Removes each contact in turn, then carries on whatever came of it: the client asked for
// all of them and a partial answer is better than none.
void Registrar::_remove_then(std::shared_ptr<types::Account> account, std::shared_ptr<Channel> channel, std::vector<std::shared_ptr<types::SIPUri>> contacts,
                             std::size_t index, std::function<void()> then) {
  auto core = _core.lock();
  if (!core) return;
  if (index >= contacts.size()) return then();

  core->qualifier()->forget(account->identity->uri->to_string(), contacts[index]);

  auto self = shared_from_this();
  core->account_unregister(account, contacts[index], channel, [this, self, account, channel, contacts, index, then](plugins::Status) mutable {
    _remove_then(account, channel, std::move(contacts), index + 1, std::move(then));
  });
}

void Registrar::_store_binding(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                               std::shared_ptr<types::Account> account, std::shared_ptr<std::vector<Binding>> bindings, std::size_t index,
                               std::uint32_t expires_seconds, Binding binding, std::shared_ptr<Channel> channel) {
  auto core = _core.lock();
  if (!core) return;

  auto self = shared_from_this();

  auto next = [this, self, request, transaction, account, bindings, index, expires_seconds]() {
    _write_bindings(request, transaction, account, bindings, index + 1, expires_seconds);
  };

  if (binding.expires == 0) {
    // A removal that fails is logged and the rest still go: the client asked for all of
    // them and a partial answer is better than none.
    core->qualifier()->forget(account->identity->uri->to_string(), binding.contact);
    return core->account_unregister(account, binding.contact, channel, [next](plugins::Status) { next(); });
  }

  core->account_register(
      account, binding.contact, channel, binding.expires, _path_of(request),
      [this, self, request, transaction, account, binding, channel, next](plugins::Status status) {
        if (!status.ok) {
          _logger->error("REGISTER could not store the binding for " + account->identity->to_string() + " - " + status.error);
          return _send_status(transaction, request, 500, "Server Internal Error");
        }

        if (auto core = _core.lock(); core && channel) {
          core->qualifier()->watch(account->identity->uri->to_string(), binding.contact, channel->flow_id(), binding.qualify, binding.expires);
        }

        next();
      },
      binding.instance, binding.reg_id);
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

  // "If there is neither, a locally-configured default value MUST be taken as the
  // requested expiration." The realm's own maximum is that default.
  return realm && realm->registration_timeout > 0 ? realm->registration_timeout : kDefaultMaximumExpiry;
}

std::uint32_t Registrar::_granted_expiry(std::uint32_t requested, const std::shared_ptr<types::Realm>& realm) const {
  const std::uint32_t maximum = realm && realm->registration_timeout > 0 ? realm->registration_timeout : kDefaultMaximumExpiry;
  return std::min(requested, maximum);
}

bool Registrar::_is_too_brief(std::uint32_t requested, const std::shared_ptr<types::Realm>& realm) const {
  if (!realm || realm->registration_minimum == 0) return false;

  // "If and only if the requested expiration interval is greater than zero AND smaller
  // than one hour AND less than a registrar-configured minimum". Zero is a removal, not
  // a short registration, and an hour is long enough by the RFC's own reckoning however
  // the realm is configured.
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

  const auto local = channel->_connection->local_endpoint();
  const auto transport = Util::to_lower(channel->_connection->transport_name());

  auto route = std::make_shared<types::SIPUri>();
  route->valid = true;
  route->scheme = (transport == "tls" || transport == "wss") ? "sips" : "sip";

  // The address the node was told to advertise, when it was told one. A UDP listener is
  // bound to the wildcard, so its local endpoint is 0.0.0.0 - and a Service-Route
  // pointing at 0.0.0.0 is a route the client cannot use, which is worse than sending
  // none at all. The same rule now decides the Via and the Record-Route.
  route->host = core->advertised_address(local.address().to_string());
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

      // RFC 5626 section 6: an outbound binding is listed with what identifies it.
      if (location.reg_id != 0) {
        contact->tags["+sip.instance"] = "\"" + location.instance + "\"";
        contact->tags["reg-id"] = std::to_string(location.reg_id);
      }

      response->header->add("Contact", std::make_shared<SIPIdentityHeader>(contact));
    }

    response->header->add("Expires", std::make_shared<UIntHeader>(expires_seconds));

    // RFC 5626 section 6: a registrar that honoured outbound for a contact in the request
    // says so. No Flow-Timer: it is optional, and the one value this node could give - how
    // long it keeps an idle UDP flow - is longer than most NATs keep theirs, so the client's
    // own default keep-alive interval is the better one.
    bool honoured = false;
    for (const auto& header : request->header->headers_map["Contact"]) {
      auto contact = header->as<SIPIdentityHeader>();
      bool malformed = false;
      if (contact != nullptr && contact->value != nullptr && outbound_of(request, *contact->value, malformed)) honoured = true;
    }
    if (honoured) response->header->add("Require", std::make_shared<headers::StringHeader>("outbound"));

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

void Registrar::_send_interval_too_brief(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                                         const std::shared_ptr<types::Realm>& realm) {
  auto response = request->generate_response();
  response->header->response_code = 423;
  response->header->response_message = "Interval Too Brief";

  // "This response MUST contain a Min-Expires header field that states the minimum
  // expiration interval the registrar is willing to honor." Without it the client has
  // nothing to retry with, which is the whole reason 423 is not just a 400.
  response->header->add("Min-Expires", std::make_shared<UIntHeader>(realm ? realm->registration_minimum : 0));

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

    digest::add_challenges(*response->header, "WWW-Authenticate", realm->name, nonce.value);

    transaction->send(response);
  });
}

}  // namespace athenasip
