//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "builtin_policy.h"

#include <utility>

#include "../config.h"
#include "../headers/sip_identity_header.h"
#include "../loggers/logger_scoped.h"
#include "../util.h"

namespace athenasip::policy {

namespace {

using headers::SIPIdentityHeader;

std::shared_ptr<types::SIPUri> uri_of(const std::shared_ptr<SIPMessage>& message, const std::string& field) {
  if (!message->header->contains(field)) return nullptr;

  auto identity = message->header->headers_map[field][0]->as<SIPIdentityHeader>();
  if (identity == nullptr || identity->value == nullptr) return nullptr;
  return identity->value->uri;
}

bool has_to_tag(const std::shared_ptr<SIPMessage>& message) {
  if (!message->header->contains("To")) return false;

  auto identity = message->header->headers_map["To"][0]->as<SIPIdentityHeader>();
  return identity != nullptr && identity->value != nullptr && identity->value->tags.count("tag") != 0;
}

template <typename T>
plugins::Result<T> decided(T decision) {
  return plugins::Result<T>::success(std::move(decision));
}

template <typename T>
plugins::Result<T> undecided(std::string reason) {
  return plugins::Result<T>::failure(std::move(reason));
}

}  // namespace

BuiltinPolicy::BuiltinPolicy(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url)
    : _logger(std::make_shared<loggers::LoggerScoped>("policy", std::move(logger))) {
  (void)url;
}

// This node is not an open relay. A caller claiming one of this node's realms must authenticate; any other
// caller may only reach this node's realms.
void BuiltinPolicy::authorize(plugins::Executor on, std::shared_ptr<RequestView> request, plugins::Handler<AuthDecision> handler) {
  const auto& message = request->message;
  const auto& method = message->header->request_method;

  // RFC 3261 22.1: ACK and CANCEL cannot be challenged.
  if (method == "ACK" || method == "CANCEL") return _complete(on, handler, decided(AuthDecision::accept()));

  // A peer node has already authorised the caller. Trust comes from the certificate, never from a Via or a Route.
  if (request->from_peer) return _complete(on, handler, decided(AuthDecision::accept()));

  // In-dialog requests of an authorised call pass. The dialog table decides; a To tag alone proves nothing.
  if (has_to_tag(message) && message->in_known_dialog) return _complete(on, handler, decided(AuthDecision::accept()));

  // Routed back through a Path or Record-Route this node wrote: nobody else could have sealed the token.
  if (request->valid_flow_token) return _complete(on, handler, decided(AuthDecision::accept()));

  // RFC 3261 10.3 step 1: the From is the foreign address of record, so a subscriber of any realm here may relay.
  if (request->relay) return _complete(on, handler, decided(AuthDecision::digest(nullptr, false)));

  auto caller = uri_of(message, "From");
  if (caller == nullptr) {
    _logger->info("Request with no usable From - 400");
    return _complete(on, handler, decided(AuthDecision::reject(400, "Bad Request")));
  }

  auto host = _host;
  _host->realm(Util::to_lower(caller->host), [this, on, handler, request, caller, host](plugins::Result<std::shared_ptr<types::Realm>> found) {
    if (!found.ok)
      return _complete(on, handler, undecided<AuthDecision>("could not read the realm of the caller " + caller->to_string() + " - " + found.error));

    if (found.value) return _complete(on, handler, decided(AuthDecision::digest(found.value)));

    // An unknown caller may not route a request off this node.
    if (request->has_route) {
      _logger->info("Request from " + caller->to_string() + " routed off this node - 403");
      return _complete(on, handler, decided(AuthDecision::reject(403, "Forbidden")));
    }

    const auto called = request->message->header->request_uri;
    host->realm(Util::to_lower(called->host), [this, on, handler, caller, called](plugins::Result<std::shared_ptr<types::Realm>> target) {
      if (!target.ok) return _complete(on, handler, undecided<AuthDecision>("could not read the realm for " + called->to_string() + " - " + target.error));

      // Anybody may call into this node's realms.
      if (target.value) return _complete(on, handler, decided(AuthDecision::accept()));

      _logger->info("Request from " + caller->to_string() + " to " + called->to_string() + ", neither of them here - 403");
      _complete(on, handler, decided(AuthDecision::reject(403, "Forbidden")));
    });
  });
}

// RFC 3261 16.5: a Request-URI in a realm here is an address of record, whose bindings are the targets. Any
// other is itself the only target.
void BuiltinPolicy::route(plugins::Executor on, std::shared_ptr<RequestView> request, plugins::Handler<RouteDecision> handler) {
  const auto called = request->message->header->request_uri;
  auto host = _host;

  _host->realm(Util::to_lower(called->host), [this, on, handler, called, host](plugins::Result<std::shared_ptr<types::Realm>> found) {
    if (!found.ok) return _complete(on, handler, undecided<RouteDecision>("could not read the realm for " + called->to_string() + " - " + found.error));

    const auto& config = host->config();

    if (!found.value) {
      auto decision = RouteDecision::forward({Target::to(called)});
      decision.media = config.behaviour;
      decision.rewrite_contact = config.behaviour_rewrite_contact;
      return _complete(on, handler, decided(std::move(decision)));
    }

    auto realm = found.value;
    auto identity = std::make_shared<types::SIPIdentity>(called->to_string());

    host->subscriber(identity, [this, on, handler, called, host, realm](plugins::Result<std::shared_ptr<types::Subscriber>> subscriber) {
      if (!subscriber.ok) {
        return _complete(on, handler, undecided<RouteDecision>("could not read the subscriber for " + called->to_string() + " - " + subscriber.error));
      }

      if (!subscriber.value) {
        _logger->info("No subscriber for " + called->to_string() + " - 404");
        return _complete(on, handler, decided(RouteDecision::reply(404, "Not Found")));
      }

      host->locations(subscriber.value->id, [this, on, handler, called, host, realm,
                                             subscriber = subscriber.value](plugins::Result<std::vector<types::Location>> bindings) {
        if (!bindings.ok) {
          return _complete(on, handler, undecided<RouteDecision>("could not read the bindings for " + called->to_string() + " - " + bindings.error));
        }

        if (bindings.value.empty()) {
          _logger->info("No bindings for " + called->to_string() + " - 480");
          return _complete(on, handler, decided(RouteDecision::reply(480, "Temporarily Unavailable")));
        }

        const auto& config = host->config();
        auto decision = RouteDecision::forward({Target::of(subscriber, std::move(bindings.value))});
        decision.media = realm->behaviour.over(config.behaviour);
        decision.rewrite_contact = realm->behaviour.rewrite_contact.value_or(config.behaviour_rewrite_contact);
        _complete(on, handler, decided(std::move(decision)));
      });
    });
  });
}

// RFC 3261 10.3: the realm is the To's domain. A REGISTER for a domain not served here is forwarded there when
// sip.forward_register allows (step 1); one whose Request-URI names this node or a realm here is step 5's 404.
void BuiltinPolicy::register_(plugins::Executor on, std::shared_ptr<RequestView> request, plugins::Handler<RegisterDecision> handler) {
  const auto& message = request->message;
  auto aor = uri_of(message, "To");
  if (aor == nullptr) return _complete(on, handler, decided(RegisterDecision::reject(400, "Bad Request")));

  auto host = _host;
  _host->realm(Util::to_lower(aor->host), [this, on, handler, message, aor, host](plugins::Result<std::shared_ptr<types::Realm>> found) {
    if (!found.ok) return _complete(on, handler, undecided<RegisterDecision>("could not read the realm - " + found.error));

    const auto& config = host->config();

    if (found.value) {
      const auto& realm = found.value;
      return _complete(on, handler,
                       decided(RegisterDecision::accept(realm, realm->registration_timeout, realm->registration_minimum,
                                                        realm->behaviour.qualify_over(config.behaviour_qualify_interval))));
    }

    const auto unserved = [this, aor]() {
      _logger->info("REGISTER for unserved domain " + aor->host + " - 404");
      return decided(RegisterDecision::reject(404, "Not Found"));
    };

    const auto& uri = message->header->request_uri;
    const std::uint16_t port = uri ? uri->port.value_or(Util::to_lower(uri->scheme) == "sips" ? 5061 : 5060) : 0;
    if (!uri || host->names_this_node(uri->host, port)) return _complete(on, handler, unserved());

    host->realm(Util::to_lower(uri->host), [this, on, handler, uri, host, unserved](plugins::Result<std::shared_ptr<types::Realm>> named) {
      if (!named.ok) return _complete(on, handler, undecided<RegisterDecision>("could not read the realm - " + named.error));
      if (named.value) return _complete(on, handler, unserved());

      if (host->config().sip_forward_register != "subscribers") {
        _logger->info("REGISTER for " + uri->to_string() + ", a domain not served here, and sip.forward_register is never - 403");
        return _complete(on, handler, decided(RegisterDecision::reject(403, "Forbidden")));
      }

      _complete(on, handler, decided(RegisterDecision::forward()));
    });
  });
}

}  // namespace athenasip::policy
