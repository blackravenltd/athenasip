//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "registrar.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <ctime>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#include "channel.h"
#include "core.h"
#include "digest.h"
#include "headers/authorization_header.h"
#include "headers/cseq_header.h"
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

// A decimal number, all of the text, or nothing: "12abc" is not 12.
std::optional<std::uint64_t> number_of(const std::string& text) {
  std::uint64_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc() || parsed.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

// An expiry as the registrar holds one, 32 bits: a larger one asked for is the largest there is, which the realm's
// maximum then cuts down, never a wrap to a small number or to 0, which would remove the binding (review finding 14).
std::uint32_t expiry_of(std::uint64_t asked) { return static_cast<std::uint32_t>(std::min<std::uint64_t>(asked, std::numeric_limits<std::uint32_t>::max())); }

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

  const auto id = number_of(reg_id->second);
  if (!id || *id == 0 || *id > 0x7FFFFFFFul) {
    malformed = true;
    return std::nullopt;
  }
  return std::make_pair(value, static_cast<std::uint32_t>(*id));
}

// RFC 3261 10.3 step 7: the REGISTER's Call-ID and CSeq number, which order the updates to a binding.
std::pair<std::string, std::uint64_t> order_of(const SIPMessage& request) {
  std::string call_id;
  if (request.header->contains("Call-ID")) call_id = request.header->headers_map["Call-ID"][0]->to_string();
  std::uint64_t cseq = 0;
  if (request.header->contains("CSeq")) {
    if (const auto header = request.header->headers_map["CSeq"][0]->as<headers::CSeqHeader>()) cseq = header->sequence;
  }
  return {call_id, cseq};
}

// Whether a stored binding is the one a contact in a REGISTER names: the same flow for outbound (RFC 5626 6),
// otherwise the same contact as the store keys it, by user, host and port.
bool same_binding(const types::Location& location, const std::shared_ptr<types::SIPUri>& contact, const std::string& instance, std::uint32_t reg_id) {
  if (reg_id != 0) return location.reg_id == reg_id && location.instance == instance;
  return location.contact && contact && location.reg_id == 0 && location.contact->user == contact->user &&
         Util::to_lower(location.contact->host) == Util::to_lower(contact->host) && location.contact->port.value_or(0) == contact->port.value_or(0);
}

// Step 7: a binding last written from this Call-ID with a CSeq as high or higher is a REGISTER out of order.
bool out_of_order(const types::Location& location, const std::pair<std::string, std::uint64_t>& order) {
  return !order.first.empty() && location.call_id == order.first && order.second <= location.cseq;
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

  auto view = std::make_shared<policy::RequestView>();
  view->message = request;

  auto self = shared_from_this();
  core->policy()->register_(core->strand(), view, [this, self, request, transaction, aor](plugins::Result<policy::RegisterDecision> decided) {
    auto core = _core.lock();
    if (!core) return;

    // A datastore failure, or a policy that could not decide, is a 500.
    if (!decided.ok) {
      _logger->error("REGISTER could not be decided - " + decided.error);
      return _send_status(transaction, request, 500, "Server Internal Error");
    }

    const auto& decision = decided.value;
    switch (decision.kind) {
      case policy::RegisterDecision::Kind::Reject:
        return _send_status(transaction, request, decision.code, decision.reason);
      case policy::RegisterDecision::Kind::Forward:
        return core->register_forward(request, transaction);
      case policy::RegisterDecision::Kind::Accept:
        if (!decision.realm) {
          _logger->error("REGISTER accepted by the policy with no realm - 500");
          return _send_status(transaction, request, 500, "Server Internal Error");
        }
        return _on_realm(request, transaction, aor, decision.realm,
                         Terms{decision.max_expires, decision.min_expires, decision.qualify_interval,
                               decision.realm ? decision.realm->registration_max_bindings : 0u});
    }
  });
}

// RFC 3261 10.3 steps 4 to 6. An unknown subscriber in a served realm is challenged, so REGISTER cannot
// enumerate subscribers.
void Registrar::_on_realm(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                          std::shared_ptr<types::SIPIdentity> aor, std::shared_ptr<types::Realm> realm, Terms terms) {
  auto core = _core.lock();
  if (!core) return;

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

  // An address of record guessed at from many addresses is closed to strangers for a while (AuthGuard).
  if (const auto closed = core->auth_closed(*request, aor->uri->canonical_address())) {
    _logger->info("REGISTER for " + aor->to_string() + ", closed to this address after repeated failures - 403");
    return _send_refusal(transaction, request, *closed);
  }

  switch (digest::nonce_state(*realm, auth->fields["nonce"], Core::source_of(*request))) {
    case digest::NonceState::bad:
      _logger->info("REGISTER with a nonce this realm did not mint - challenging");
      return _send_challenge(transaction, request, realm);
    case digest::NonceState::stale:
      // RFC 7616 3.3: the client signs a fresh nonce without asking its user again.
      _logger->info("REGISTER with an expired nonce - challenging as stale");
      return _send_challenge(transaction, request, realm, true);
    case digest::NonceState::good:
      break;
  }

  auto self = shared_from_this();
  core->subscriber_get(aor, [this, self, request, transaction, aor, realm, auth, terms](plugins::Result<std::shared_ptr<types::Subscriber>> found) {
    if (!found.ok) {
      _logger->error("REGISTER could not read the subscriber - " + found.error);
      return _send_status(transaction, request, 500, "Server Internal Error");
    }

    _on_subscriber(request, transaction, aor, realm, auth, found.value, terms);
  });
}

void Registrar::_on_subscriber(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                               std::shared_ptr<types::SIPIdentity> aor, std::shared_ptr<types::Realm> realm, std::shared_ptr<types::Authorization> auth,
                               std::shared_ptr<types::Subscriber> subscriber, Terms terms) {
  if (!subscriber) {
    _logger->info("REGISTER for unknown subscriber " + aor->to_string() + " - challenging");
    if (auto core = _core.lock()) core->auth_failed(*request, aor->uri->canonical_address(), "no such subscriber");
    return _send_challenge(transaction, request, realm);
  }

  if (const auto why = digest::verify(*subscriber, *auth, request->header->request_method, request->header->request_uri->to_string()); !why.empty()) {
    _logger->info("REGISTER for " + aor->to_string() + " with " + why + " - challenging");
    if (auto core = _core.lock()) core->auth_failed(*request, aor->uri->canonical_address(), why);
    return _send_challenge(transaction, request, realm);
  }

  // A right answer seen before is a replay (RFC 7616 5.6): stale, so a client whose own nc slipped signs again.
  if (auto core = _core.lock(); !core || !core->nonce_counts().accept(*auth, std::time(nullptr) + realm->nonce_expiry)) {
    _logger->info("REGISTER for " + aor->to_string() + " replays an answer - challenging as stale");
    return _send_challenge(transaction, request, realm, true);
  }

  if (auto core = _core.lock()) core->auth_succeeded(*request, aor->uri->canonical_address());
  request->authenticated = true;

  // The connection now belongs to this subscriber; see Channel::authenticated_as.
  if (auto channel = request->channel.lock()) channel->authenticated_as(aor->uri->to_string());

  _apply_bindings(request, transaction, subscriber, terms);
}

void Registrar::_apply_bindings(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction,
                                std::shared_ptr<types::Subscriber> subscriber, Terms terms) {
  auto core = _core.lock();
  if (!core) return;

  const auto requested = _requested_expiry(request, terms);
  const auto expires = _granted_expiry(requested, terms);

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

      // Step 6 with step 7's ordering: a binding this Call-ID last wrote with a CSeq as high or higher stays.
      const auto order = order_of(*request);
      auto bindings = std::make_shared<std::vector<Binding>>();
      for (const auto& location : found.value) {
        if (location.contact && !out_of_order(location, order)) bindings->push_back(Binding{location.contact, 0});
      }

      _write_bindings(request, transaction, subscriber, bindings, 0, 0);
    });
  }

  auto bindings = std::make_shared<std::vector<Binding>>();
  const auto qualify = terms.qualify_interval;
  const bool pushed_elsewhere = pushed_by_another_proxy(request);

  for (const auto& header : request->header->headers_map["Contact"]) {
    auto contact = header->as<SIPIdentityHeader>();
    if (contact == nullptr || contact->value == nullptr || contact->value->uri == nullptr) continue;

    // A per-contact expires parameter overrides the Expires header for that one binding.
    auto contact_requested = requested;
    const auto tag = contact->value->tags.find("expires");
    if (tag != contact->value->tags.end()) {
      const auto asked = number_of(tag->second);
      if (!asked) return _send_status(transaction, request, 400, "Bad Request");
      contact_requested = expiry_of(*asked);
    }

    // Step 7: the first contact that is too brief refuses the whole request.
    if (_is_too_brief(contact_requested, terms)) {
      _logger->info("REGISTER asked for " + std::to_string(contact_requested) + "s, below the realm minimum - 423");
      return _send_interval_too_brief(transaction, request, terms.min_expires);
    }

    Binding binding{contact->value->uri, _granted_expiry(contact_requested, terms), qualify};

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
          return _send_interval_too_brief(transaction, request, std::max(minimum, terms.min_expires));
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

  // RFC 3261 10.3 step 7: an update from the Call-ID that last wrote a binding needs a higher CSeq; one that is not
  // higher is a REGISTER out of order, late over UDP or replayed by a failover, and would undo a newer one. The
  // whole request fails, as a request out of order in a dialog does (12.2.2).
  core->location_list(subscriber->id, [this, self, request, transaction, subscriber, bindings, expires,
                                       terms](plugins::Result<std::vector<types::Location>> found) {
    if (found.ok) {
      const auto order = order_of(*request);
      for (const auto& location : found.value) {
        for (const auto& binding : *bindings) {
          if (!same_binding(location, binding.contact, binding.instance, binding.reg_id) || !out_of_order(location, order)) continue;
          _logger->info("REGISTER for " + subscriber->identity->to_string() + " with CSeq " + std::to_string(order.second) + ", not above the " +
                        std::to_string(location.cseq) + " that last wrote " + binding.contact->to_string() + " - 500");
          return _send_status(transaction, request, 500, "Server Internal Error");
        }
      }

      // The realm's cap on bindings (review finding 15): what the AOR would hold once this request is applied, each
      // contact replacing or removing the binding it names and adding one where it names none.
      if (terms.max_bindings != 0) {
        std::size_t after = found.value.size();
        for (const auto& binding : *bindings) {
          const bool held = std::any_of(found.value.begin(), found.value.end(), [&binding](const types::Location& location) {
            return same_binding(location, binding.contact, binding.instance, binding.reg_id);
          });
          if (held && binding.expires == 0) --after;
          if (!held && binding.expires != 0) ++after;
        }
        if (after > terms.max_bindings) {
          _logger->info("REGISTER for " + subscriber->identity->to_string() + " would hold " + std::to_string(after) + " bindings, above the " +
                        std::to_string(terms.max_bindings) + " allowed - 403");
          return _send_status(transaction, request, 403, "Too Many Bindings");
        }
      }
    }

    _write_bindings(request, transaction, subscriber, bindings, 0, expires);
  });
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
      binding.instance, binding.reg_id, binding.push, order_of(*request).first, order_of(*request).second);
}

// RFC 3261 10.2.1: the Expires header field is the default for every contact that has no expires parameter of its own.
// A contact's own parameter is that contact's alone (the binding loop reads it), never the default for the others:
// `<a>;expires=0, <b>` removes a and registers b.
std::uint32_t Registrar::_requested_expiry(const std::shared_ptr<SIPMessage>& request, const Terms& terms) const {
  if (request->header->contains("Expires")) {
    auto expires = request->header->headers_map["Expires"][0]->as<UIntHeader>();
    if (expires != nullptr) return expiry_of(expires->value);
  }

  // RFC 3261 10.3 step 7: with neither, the local default applies, which is the realm's
  // maximum.
  return terms.max_expires > 0 ? terms.max_expires : kDefaultMaximumExpiry;
}

std::uint32_t Registrar::_granted_expiry(std::uint32_t requested, const Terms& terms) const {
  const std::uint32_t maximum = terms.max_expires > 0 ? terms.max_expires : kDefaultMaximumExpiry;
  return std::min(requested, maximum);
}

bool Registrar::_is_too_brief(std::uint32_t requested, const Terms& terms) const {
  if (terms.min_expires == 0) return false;

  // RFC 3261 10.3 step 7: too brief only if greater than zero (zero is a removal), under an
  // hour, and below the realm's minimum.
  return requested > 0 && requested < kNeverTooBrief && requested < terms.min_expires;
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

    for (const auto& value : _alternate_servers(request, expires_seconds)) {
      response->header->add("AthenaSIP-Alternate-Server", std::make_shared<headers::StringHeader>(value));
    }

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

std::vector<std::string> Registrar::_alternate_servers(const std::shared_ptr<SIPMessage>& request, std::uint32_t expires_seconds) const {
  auto core = _core.lock();
  auto channel = request->channel.lock();
  if (!core || !channel || !channel->_connection || expires_seconds == 0 || !supports(request->header, "athenasip-failover")) return {};

  // Only where the transport authenticated this node to the client: anywhere else the header is a redirection
  // whoever can forge a response could send.
  const auto transport = Util::to_lower(channel->_connection->transport_name());
  if (transport != "tls" && transport != "wss") return {};

  const auto interval = core->config->events_status_interval;
  const auto stale_after = interval == 0 ? std::chrono::seconds::max() : std::chrono::seconds(interval * 3);

  // The discovery bus's answer, as /api/v1/nodes gives it: the other nodes that are up, on the same transport.
  // The list is good for as long as the registration, and the client learns it afresh when it refreshes.
  std::vector<std::string> values;
  for (const auto& node : core->nodes()->list(stale_after)) {
    if (node.id == core->config->sip_node_id || node.stale || node.status != "ok") continue;

    for (const auto& entry : node.transports) {
      if (!entry.is_object()) continue;
      const auto* kind = entry.as_object().if_contains("transport");
      const auto* uri = entry.as_object().if_contains("uri");
      if (kind == nullptr || uri == nullptr || !kind->is_string() || !uri->is_string() || kind->as_string() != transport) continue;

      values.push_back("<" + std::string(uri->as_string()) + ">;expires=" + std::to_string(expires_seconds));
    }
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
                                const std::shared_ptr<types::Realm>& realm, bool stale) {
  // Challenges to one address are limited (AuthGuard): past that it is refused, not challenged.
  if (auto core = _core.lock(); core && !core->auth_may_challenge(*request)) {
    _logger->info("REGISTER from an address past its challenges - 403");
    return _send_refusal(transaction, request, std::chrono::seconds(60));
  }

  auto response = request->generate_response();
  response->header->response_code = 401;
  response->header->response_message = "Unauthorized";
  response->header->add("Reason", "SIP;cause=401;text=\"Unauthorized\"");

  // Nothing is kept for the nonce: it carries its own proof, bound to the address it goes to (digest::nonce_mint).
  if (realm) digest::add_challenges(*response->header, "WWW-Authenticate", realm->name, digest::nonce_mint(*realm, Core::source_of(*request)), stale);

  transaction->send(response);
}

// RFC 3261 21.4.4 with 20.33: refused for now, and when to try again.
void Registrar::_send_refusal(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                              std::chrono::seconds retry_after) {
  auto response = request->generate_response();
  response->header->response_code = 403;
  response->header->response_message = "Forbidden";
  response->header->add("Retry-After", std::to_string(retry_after.count()));
  transaction->send(response);
}

}  // namespace athenasip
