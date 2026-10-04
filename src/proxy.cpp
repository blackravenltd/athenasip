//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "proxy.h"

#include <chrono>
#include <set>
#include <utility>

#include "call.h"
#include "channel.h"
#include "core.h"
#include "digest.h"
#include "headers/authorization_header.h"
#include "headers/cseq_header.h"
#include "headers/session_expires_header.h"
#include "headers/sip_identity_header.h"
#include "headers/string_header.h"
#include "headers/uint_header.h"
#include "headers/via_header.h"
#include "loggers/logger_scoped.h"
#include "media/media_engine.h"
#include "push/push_parameters.h"
#include "qualifier.h"
#include "types/location.h"
#include "util.h"

namespace athenasip {

using athenasip::headers::CSeqHeader;
using athenasip::headers::SIPIdentityHeader;
using athenasip::headers::UIntHeader;
using athenasip::headers::ViaHeader;

namespace {

constexpr std::uint64_t kDefaultMaxForwards = 70;

// Branch = kMagicCookie + loop token + "." + a unique value (RFC 3261 8.1.1.7, 16.6 step 8), so that 16.3.4
// can read the token back from a Via this node wrote.
constexpr const char* kMagicCookie = "z9hG4bK";
constexpr std::size_t kLoopTokenLength = 16;

// RFC 3261 18.1.1: the largest request sent over UDP. The path MTU is never discovered, so this always
// applies.
constexpr std::size_t kMaxUdpRequest = 1300;

bool is_2xx(int code) { return code >= 200 && code < 300; }
bool is_final(int code) { return code >= 200; }

// RFC 3261 20.42 sent-by: host [":" port]. An IPv6 reference keeps its brackets.
std::pair<std::string, std::uint16_t> split_sent_by(const std::string& sent_by, std::uint16_t default_port) {
  const auto bracket = sent_by.rfind(']');
  const auto colon = sent_by.rfind(':');

  if (colon == std::string::npos || (bracket != std::string::npos && colon < bracket)) return {sent_by, default_port};

  const auto port = sent_by.substr(colon + 1);
  if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos) return {sent_by, default_port};

  return {sent_by.substr(0, colon), static_cast<std::uint16_t>(std::stoul(port))};
}

// The transport a Via names, lower case: "SIP/2.0/UDP" -> "udp".
std::string transport_of_via(const ViaHeader& via) {
  const auto slash = via.version.rfind('/');
  if (slash == std::string::npos) return "udp";
  return Util::to_lower(via.version.substr(slash + 1));
}

std::string tag_of(const std::shared_ptr<SIPMessage>& message, const std::string& field) {
  if (!message->header->contains(field)) return "";

  auto identity = message->header->headers_map[field][0]->as<SIPIdentityHeader>();
  if (identity == nullptr || identity->value == nullptr) return "";

  const auto tag = identity->value->tags.find("tag");
  return tag == identity->value->tags.end() ? "" : tag->second;
}

std::string value_of(const std::shared_ptr<SIPMessage>& message, const std::string& field) {
  if (!message->header->contains(field)) return "";
  return message->header->headers_map[field][0]->to_string();
}

// The URI of a Route or Record-Route value, or null if it is not a name-addr.
std::shared_ptr<SIPUri> route_uri(const std::shared_ptr<headers::Header>& header) {
  auto identity = header->as<SIPIdentityHeader>();
  if (identity == nullptr || identity->value == nullptr) return nullptr;
  return identity->value->uri;
}

// RFC 3261 20.15: a body is SDP only when Content-Type says so.
bool has_sdp(const std::shared_ptr<SIPMessage>& message) {
  if (message->body.empty() || !message->header->contains("Content-Type")) return false;
  return Util::to_lower(message->header->headers_map["Content-Type"][0]->to_string()).rfind("application/sdp", 0) == 0;
}

// The size of the message on the wire. Sets Content-Length first, as the transport will.
std::size_t wire_size(const std::shared_ptr<SIPMessage>& message) {
  message->header->clear("Content-Length");
  message->header->add("Content-Length", std::make_shared<UIntHeader>(message->body.size()));
  return message->to_string().size();
}

// RFC 3261 18.1.1: the top Via must name the transport the request actually leaves on.
void set_top_via_transport(const std::shared_ptr<SIPMessage>& message, const std::string& transport) {
  if (!message->header->contains("Via")) return;

  auto via = message->header->headers_map["Via"][0]->as<ViaHeader>();
  if (via == nullptr) return;

  const auto slash = via->version.rfind('/');
  via->version = (slash == std::string::npos ? std::string("SIP/2.0") : via->version.substr(0, slash)) + "/" + transport;
}

// RFC 4028 sections 4 and 5: Session-Expires and Min-SE share a header type.
headers::SessionExpiresHeader* session_field_of(const std::shared_ptr<SIPMessage>& message, const std::string& field) {
  if (!message->header->contains(field)) return nullptr;
  return message->header->headers_map[field][0]->as<headers::SessionExpiresHeader>();
}

// RFC 3261 20.32, 20.37: whether a Require or Supported field carries an option tag.
bool has_option_tag(const std::shared_ptr<SIPMessage>& message, const std::string& field, const std::string& tag) {
  if (!message->header->contains(field)) return false;

  for (const auto& value : message->header->headers_map[field]) {
    if (Util::to_lower(Util::trim(value->to_string())) == tag) return true;
  }

  return false;
}

// RFC 4028 section 4: only INVITE and UPDATE negotiate a session interval.
bool is_session_refresh(const std::string& method) { return method == "INVITE" || method == "UPDATE"; }

// The loop token of a branch this node wrote, or empty.
std::string branch_loop_token(const std::string& branch) {
  const std::string cookie = kMagicCookie;
  if (branch.rfind(cookie, 0) != 0) return "";

  const auto rest = branch.substr(cookie.size());
  const auto dot = rest.find('.');
  if (dot != kLoopTokenLength) return "";

  return rest.substr(0, kLoopTokenLength);
}

}  // namespace

Proxy::Proxy(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Core> core)
    : _logger(std::make_shared<loggers::LoggerScoped>("proxy", std::move(logger))), _core(core) {}

void Proxy::on_request(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction) {
  auto core = _core.lock();
  if (!core) return;

  if (!request->header->request_uri) {
    _logger->info("Request with no Request-URI - 400");
    return _send_status(transaction, request, 400, "Bad Request");
  }

  // RFC 3261 16.3.4: computed before route preprocessing changes the fields it hashes.
  const auto token = _loop_token(request);

  if (_is_loop(request, token)) {
    _logger->info("Request has been here before with nothing changed - 482");
    return _send_status(transaction, request, 482, "Loop Detected");
  }

  // RFC 3261 11.2: an OPTIONS for this node is answered by it. 16.3 step 3: so is one that has run out of hops.
  if (request->header->request_method == "OPTIONS") {
    const auto& uri = request->header->request_uri;
    auto max_forwards = request->header->contains("Max-Forwards") ? request->header->headers_map["Max-Forwards"][0]->as<UIntHeader>() : nullptr;
    if (max_forwards != nullptr && max_forwards->value == 0) return _answer_options(transaction, request);

    if (uri->user.empty()) {
      const auto discovered = core->address_discovery()->finding();
      if (_names_this_node(*uri) || (discovered && discovered->address == uri->host)) return _answer_options(transaction, request);

      // A realm served here, with no user, is this node as well.
      auto self = shared_from_this();
      return core->realm_get_by_name(Util::to_lower(uri->host),
                                     [this, self, request, transaction, token](plugins::Result<std::shared_ptr<types::Realm>> found) {
                                       if (found.ok && found.value) return _answer_options(transaction, request);
                                       _proceed(request, transaction, token);
                                     });
    }
  }

  _proceed(request, transaction, token);
}

void Proxy::_proceed(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction, const std::string& token) {
  // RFC 4028 8.1, before any copy is forwarded.
  if (!_apply_session_timer(request, transaction)) return;

  // RFC 3261 16.4, then authorization, then 16.5. Routes first: a route set naming another host takes the
  // request off this node.
  _preprocess_routes(request);

  auto self = shared_from_this();
  _authorize(request, transaction, [this, self, request, transaction, token]() { _determine_targets(request, transaction, token); });
}

// This node is not an open relay. A caller claiming one of this node's realms must authenticate; any other
// caller may only reach this node's realms.
void Proxy::_authorize(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction,
                       std::function<void()> then) {
  auto core = _core.lock();
  if (!core) return;

  // RFC 3261 22.1: ACK and CANCEL cannot be challenged.
  const auto& method = request->header->request_method;
  if (method == "ACK" || method == "CANCEL") return then();

  // A peer node, admitted by the inter-node listener on a cluster certificate, has already authorized the
  // caller. Trust comes from the certificate, never from a Via or a Route.
  if (const auto channel = request->channel.lock(); channel && !channel->peer_node().empty()) return then();

  // In-dialog requests of an authorized call pass. The dialog table decides; a To tag alone proves nothing.
  if (!tag_of(request, "To").empty() && request->in_known_dialog) return then();

  // A request routed back through a Path or Record-Route this node wrote passes: the flow token in it is sealed
  // by this node (FlowTokens), so nobody else could have made it.
  if (!request->flow_token.empty() && !core->flow_tokens().open(request->flow_token).empty()) return then();

  auto from = request->header->contains("From") ? request->header->headers_map["From"][0]->as<SIPIdentityHeader>() : nullptr;
  if (from == nullptr || from->value == nullptr || from->value->uri == nullptr) {
    _logger->info("Request with no usable From - 400");
    return _send_status(transaction, request, 400, "Bad Request");
  }

  auto caller = from->value->uri;
  auto self = shared_from_this();

  core->realm_get_by_name(Util::to_lower(caller->host), [this, self, request, transaction, caller, then](plugins::Result<std::shared_ptr<types::Realm>> found) {
    auto core = _core.lock();
    if (!core) return;

    if (!found.ok) {
      _logger->error("Could not read the realm of the caller " + caller->to_string() + " - " + found.error);
      return _send_status(transaction, request, 500, "Server Internal Error");
    }

    if (found.value) return _authenticate(request, transaction, found.value, caller, then);

    // An unknown caller may not route a request off this node.
    if (request->header->contains("Route")) {
      _logger->info("Request from " + caller->to_string() + " routed off this node - 403");
      return _send_status(transaction, request, 403, "Forbidden");
    }

    core->realm_get_by_name(Util::to_lower(request->header->request_uri->host), [this, self, request, transaction, caller,
                                                                                 then](plugins::Result<std::shared_ptr<types::Realm>> target) {
      if (!target.ok) {
        _logger->error("Could not read the realm for " + request->header->request_uri->to_string() + " - " + target.error);
        return _send_status(transaction, request, 500, "Server Internal Error");
      }

      // Anybody may call into this node's realms.
      if (target.value) return then();

      _logger->info("Request from " + caller->to_string() + " to " + request->header->request_uri->to_string() + ", neither of them here - 403");
      _send_status(transaction, request, 403, "Forbidden");
    });
  });
}

// RFC 3261 22.3: authenticates a caller whose From is in a realm this node serves.
void Proxy::_authenticate(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction,
                          const std::shared_ptr<types::Realm>& realm, const std::shared_ptr<SIPUri>& caller, std::function<void()> then) {
  auto core = _core.lock();
  if (!core) return;

  // A reliable connection that a REGISTER authenticated over is trusted for that subscriber. A UDP source
  // address is not.
  auto channel = request->channel.lock();
  if (channel && channel->_connection && channel->_connection->is_reliable() && channel->is_authenticated_as(caller->to_string())) return then();

  // Only this realm's credentials; those for other proxies are left in place (22.3).
  std::shared_ptr<headers::Header> answered;
  std::shared_ptr<types::Authorization> credentials;

  if (request->header->contains("Proxy-Authorization")) {
    for (const auto& value : request->header->headers_map["Proxy-Authorization"]) {
      auto header = value->as<headers::AuthorizationHeader>();
      if (header == nullptr || header->value == nullptr || !header->value->contains_field("realm")) continue;
      if (header->value->fields["realm"] != realm->name) continue;

      answered = value;
      credentials = header->value;
      break;
    }
  }

  if (!digest::is_complete(credentials) || !credentials->contains_field("username")) {
    _logger->info("Request from " + caller->to_string() + " with no credentials for " + realm->name + " - challenging");
    return _send_proxy_challenge(transaction, request, realm);
  }

  auto self = shared_from_this();

  core->nonce_check(
      credentials->fields["nonce"], [this, self, request, transaction, realm, caller, credentials, answered, then](plugins::Result<bool> checked) {
        auto core = _core.lock();
        if (!core) return;

        if (!checked.ok) {
          _logger->error("Could not check a nonce - " + checked.error);
          return _send_status(transaction, request, 500, "Server Internal Error");
        }

        if (!checked.value) {
          _logger->info("Request from " + caller->to_string() + " with a nonce that is unknown or expired - challenging");
          return _send_proxy_challenge(transaction, request, realm);
        }

        // Verify the credentials first, then that they match the From, so each failure gets its own answer.
        auto claimed = std::make_shared<SIPIdentity>("sip:" + credentials->fields["username"] + "@" + realm->name);

        core->subscriber_get(
            claimed, [this, self, request, transaction, realm, caller, credentials, answered, then](plugins::Result<std::shared_ptr<types::Subscriber>> found) {
              if (!found.ok) {
                _logger->error("Could not read a subscriber - " + found.error);
                return _send_status(transaction, request, 500, "Server Internal Error");
              }

              // Challenged like a wrong password, so subscribers cannot be enumerated.
              if (!found.value) {
                _logger->info("Request from " + caller->to_string() + " with credentials for no subscriber - challenging");
                return _send_proxy_challenge(transaction, request, realm);
              }

              if (const auto why = digest::verify(*found.value, *credentials, request->header->request_method); !why.empty()) {
                _logger->info("Request from " + caller->to_string() + " with " + why + " - challenging");
                return _send_proxy_challenge(transaction, request, realm);
              }

              if (credentials->fields["username"] != caller->user) {
                _logger->info("Request from " + caller->to_string() + " authenticated as " + credentials->fields["username"] + " - 403");
                return _send_status(transaction, request, 403, "Forbidden");
              }

              // Remove the used credentials: they are replayable while the nonce lives.
              request->header->remove_value("Proxy-Authorization", [&answered](std::shared_ptr<headers::Header> value) { return value == answered; });
              request->authenticated = true;

              then();
            });
      });
}

void Proxy::_send_proxy_challenge(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                                  const std::shared_ptr<types::Realm>& realm) {
  auto core = _core.lock();
  if (!core) return;

  auto self = shared_from_this();

  // The nonce must be stored before it is sent, or it would fail its own check.
  core->nonce_create(realm, [this, self, transaction, request, realm](plugins::Result<std::string> nonce) {
    if (!nonce.ok) {
      _logger->error("Cannot mint a nonce for " + realm->name + " - " + nonce.error);
      return _send_status(transaction, request, 500, "Server Internal Error");
    }

    auto response = request->generate_response();
    response->header->response_code = 407;
    response->header->response_message = "Proxy Authentication Required";
    digest::add_challenges(*response->header, "Proxy-Authenticate", realm->name, nonce.value);

    if (auto core = _core.lock()) core->dialogs()->observe_response(request, response);
    transaction->send(response);
  });
}

// The RFC 3261 16.6 step 8 hash, with two departures. The topmost Via is left out: it differs on the pass
// that comes back, and would make every loop look like a spiral. The Route set is included, because it
// decides where the request goes (16.3.4).
std::string Proxy::_loop_token(const std::shared_ptr<SIPMessage>& request) const {
  std::string material = request->header->request_uri ? request->header->request_uri->to_string() : "";

  material += "|" + tag_of(request, "To");
  material += "|" + tag_of(request, "From");
  material += "|" + value_of(request, "Call-ID");

  if (request->header->contains("CSeq")) {
    auto cseq = request->header->headers_map["CSeq"][0]->as<CSeqHeader>();
    if (cseq != nullptr) material += "|" + std::to_string(cseq->sequence);
  }

  if (request->header->contains("Route")) {
    for (const auto& route : request->header->headers_map["Route"]) material += "|" + route->to_string();
  }

  return Util::md5(material).substr(0, kLoopTokenLength);
}

bool Proxy::_is_loop(const std::shared_ptr<SIPMessage>& request, const std::string& token) const {
  auto core = _core.lock();
  if (!core || !request->header->contains("Via")) return false;

  for (const auto& header : request->header->headers_map["Via"]) {
    auto via = header->as<ViaHeader>();
    if (via == nullptr) continue;

    const auto [host, port] = split_sent_by(via->host, 5060);
    if (!core->is_local_address(host, port)) continue;

    const auto branch = via->parameters.find("branch");
    if (branch == via->parameters.end()) continue;

    if (branch_loop_token(branch->second) == token) return true;
  }

  return false;
}

void Proxy::_preprocess_routes(const std::shared_ptr<SIPMessage>& request) const {
  auto& header = request->header;

  // 16.4 rule 1: a strict router put this node's Record-Route URI (lr, naming this node) in the Request-URI
  // and the real Request-URI last in the Route set. Restore it.
  if (header->request_uri && header->contains("Route") && header->request_uri->has_parameter("lr") && _names_this_node(*header->request_uri)) {
    const auto& routes = header->headers_map["Route"];
    auto last = routes.back();

    if (auto uri = route_uri(last)) {
      header->request_uri = uri;
      header->remove_value("Route", [&last](std::shared_ptr<headers::Header> value) { return value == last; });
      _logger->debug("Undid a strict router's rewrite - Request-URI is " + header->request_uri->to_string());
    }
  }

  // 16.4 rule 2: remove the Routes naming this node. There may be two, because this node record-routes twice
  // (RFC 5658 section 3.2). The last one removed faces the far end, and its flow token is how the request
  // reaches it.
  std::string flow_token;

  while (header->contains("Route")) {
    auto top = header->headers_map["Route"][0];
    auto uri = route_uri(top);

    if (!uri || !_names_this_node(*uri)) break;

    flow_token = uri->user;
    header->remove_value("Route", [&top](std::shared_ptr<headers::Header> value) { return value == top; });
  }

  request->flow_token = std::move(flow_token);
}

void Proxy::_determine_targets(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction,
                               const std::string& loop_token) {
  auto core = _core.lock();
  if (!core) return;

  auto context = std::make_shared<Context>();
  context->request = request;
  context->server = transaction;
  context->loop_token = loop_token;

  // In-dialog requests use what the call decided at setup; otherwise the server default, until a realm
  // overrides it below.
  if (auto call = core->call_get(value_of(request, "Call-ID")); call && call->rewrite_contact) {
    context->rewrite_contact = *call->rewrite_contact;
  } else {
    context->rewrite_contact = core->config->behaviour_rewrite_contact;
  }

  // RFC 4028 8.1: remembered for 8.2. Read after _apply_session_timer, so the interval is the one forwarded.
  context->session_timer_supported = has_option_tag(request, "Supported", "timer");
  if (auto* session = session_field_of(request, "Session-Expires"); session != nullptr) context->session_interval = session->delta_seconds;

  // 16.6 step 6: a remaining Route decides the hop and the Request-URI is left alone. This is how in-dialog
  // requests are routed.
  if (request->header->contains("Route")) {
    auto next_hop = route_uri(request->header->headers_map["Route"][0]);

    if (next_hop) {
      Target target;
      target.uri = request->header->request_uri;
      target.next_hop = next_hop;
      target.flow = _flow_to(*next_hop);

      context->targets.push_back(target);
      return _forward_next(context);
    }

    _logger->info("Route header that is not a name-addr - 400");
    return _send_status(transaction, request, 400, "Bad Request");
  }

  // The route set is spent. The flow token from this node's Record-Route names the flow to the far end, whose
  // Contact may not be reachable (a browser's never is).
  if (auto flow = core->channel_for_token(request->flow_token)) {
    Target target;
    target.uri = request->header->request_uri;
    target.next_hop = request->header->request_uri;
    target.flow = flow;

    context->targets.push_back(target);
    return _forward_next(context);
  }

  // The token's channel has gone. A UDP flow is only the address pair, and the far NAT still maps it, so send
  // there rather than to the Contact.
  if (!request->flow_token.empty()) {
    if (auto hop = _datagram_hop(core->flow_tokens().open(request->flow_token))) {
      Target target;
      target.uri = request->header->request_uri;
      target.next_hop = hop;
      target.flow = _flow_to(*hop);

      context->targets.push_back(target);
      return _forward_next(context);
    }
  }

  const auto host = Util::to_lower(request->header->request_uri->host);
  auto self = shared_from_this();

  // RFC 3261 16.5: a Request-URI outside this node's realms is itself the only target.
  core->realm_get_by_name(host, [this, self, context](plugins::Result<std::shared_ptr<types::Realm>> found) {
    auto core = _core.lock();
    if (!core) return;

    const auto& request = context->request;

    if (!found.ok) {
      _logger->error("Could not read the realm for " + request->header->request_uri->to_string() + " - " + found.error);
      return _send_status(context->server, request, 500, "Server Internal Error");
    }

    if (!found.value) {
      Target target;
      target.uri = request->header->request_uri;
      target.next_hop = request->header->request_uri;
      target.flow = _flow_to(*request->header->request_uri);

      context->targets.push_back(target);
      return _forward_next(context);
    }

    // The realm is in hand here, so its media policy is read once rather than per message with a body.
    context->media_policy = found.value->behaviour.over(core->config->behaviour);
    context->rewrite_contact = found.value->behaviour.rewrite_contact.value_or(core->config->behaviour_rewrite_contact);

    // Kept on the call for in-dialog requests, which look no realm up.
    if (auto call = core->call_get(value_of(request, "Call-ID"))) call->rewrite_contact = context->rewrite_contact;

    // The Request-URI is an address of record in this realm; its bindings are the target set.
    auto identity = std::make_shared<SIPIdentity>(request->header->request_uri->to_string());

    core->subscriber_get(identity, [this, self, context](plugins::Result<std::shared_ptr<types::Subscriber>> found) {
      auto core = _core.lock();
      if (!core) return;

      const auto& request = context->request;

      if (!found.ok) {
        _logger->error("Could not read the subscriber for " + request->header->request_uri->to_string() + " - " + found.error);
        return _send_status(context->server, request, 500, "Server Internal Error");
      }

      if (!found.value) {
        _logger->info("No subscriber for " + request->header->request_uri->to_string() + " - 404");
        return _send_status(context->server, request, 404, "Not Found");
      }

      auto subscriber = found.value;
      context->callee_profile = subscriber->media_profile;

      core->location_list(subscriber->id, [this, self, context, subscriber](plugins::Result<std::vector<types::Location>> bindings) {
        auto core = _core.lock();
        if (!core) return;

        const auto& request = context->request;

        if (!bindings.ok) {
          _logger->error("Could not read the bindings for " + request->header->request_uri->to_string() + " - " + bindings.error);
          return _send_status(context->server, request, 500, "Server Internal Error");
        }

        if (bindings.value.empty()) {
          _logger->info("No bindings for " + request->header->request_uri->to_string() + " - 480");
          return _send_status(context->server, request, 480, "Temporarily Unavailable");
        }

        // RFC 5626: each binding is reached down the flow it registered over.
        _add_targets(context, std::move(bindings.value));

        _read_caller_profile(context, [this, self, context]() { _forward_next(context); });
      });
    });
  });
}

// Where the realm asks for it, rewrites each Contact's host and port to the message's source address, for
// endpoints behind NAT (Asterisk's rewrite_contact, Kamailio's fix_nated_contact). WebSocket clients are
// skipped: they are reached by flow token, and their Contact is not meant to resolve (RFC 7118).
void Proxy::_rewrite_contact(const std::shared_ptr<SIPMessage>& message, const std::shared_ptr<Channel>& from) const {
  if (!from || !from->_connection || !message->header->contains("Contact")) return;

  const auto transport = Util::to_lower(from->_connection->transport_name());
  if (transport == "ws" || transport == "wss") return;

  const auto source = from->_connection->remote_endpoint();
  const auto address = source.address().to_string();
  const auto port = source.port();

  for (const auto& header : message->header->headers_map["Contact"]) {
    auto contact = header->as<SIPIdentityHeader>();
    if (contact == nullptr || contact->value == nullptr || contact->value->uri == nullptr) continue;

    auto& uri = contact->value->uri;
    if (uri->host == address && uri->port.value_or(5060) == port) continue;

    _logger->debug("Contact " + uri->to_string() + " rewritten to " + address + ":" + std::to_string(port));
    uri->host = source.address().is_v6() ? "[" + address + "]" : address;
    uri->port = port;
  }
}

// RFC 3264 section 5: an INVITE without SDP makes the callee's 200 the offer, produced for the caller. Only
// then is the caller's subscriber profile needed. A failed read is treated as no profile.
void Proxy::_read_caller_profile(const std::shared_ptr<Context>& context, std::function<void()> then) {
  auto core = _core.lock();
  const auto& request = context->request;
  if (!core || request->header->request_method != "INVITE" || has_sdp(request) || !request->header->contains("From")) return then();

  auto from = request->header->headers_map["From"][0]->as<SIPIdentityHeader>();
  if (from == nullptr || from->value == nullptr || from->value->uri == nullptr) return then();

  auto identity = std::make_shared<SIPIdentity>(from->value->uri->to_string());
  core->subscriber_get(identity, [context, then = std::move(then)](plugins::Result<std::shared_ptr<types::Subscriber>> found) {
    if (found.ok && found.value) context->caller_profile = found.value->media_profile;
    then();
  });
}

void Proxy::_forward_next(const std::shared_ptr<Context>& context) {
  auto core = _core.lock();
  if (!core) return;

  // 16.10: a cancelled or answered fork tries no further targets.
  if (context->answered || context->cancelled || context->next >= context->targets.size()) return _send_best(context);

  // By value: the continuations below outlive this frame, and the target set may change.
  const Target target = context->targets[context->next++];

  // RFC 8599 5.6.2: a request for a new dialog, or a standalone one, waits for a push binding's client to wake.
  // An in-dialog request carries no pn-* parameters and is routed as usual.
  if (target.push && tag_of(context->request, "To").empty()) return _push_and_wait(context, target);

  _forward_target(context, target);
}

void Proxy::_forward_target(const std::shared_ptr<Context>& context, const Target& target) {
  auto core = _core.lock();
  if (!core) return;

  context->hops_left.clear();
  context->hop_target.reset();
  context->current = target;
  context->offered.reset();

  // RFC 5626 5.3: a dead outbound flow fails, and the client's next flow takes its place.
  if (target.dead) {
    _logger->info("The outbound flow for " + target.uri->to_string() + " has gone - trying the client's next flow");
    _try_other_flow(context);
    return _forward_next(context);
  }

  if (auto channel = target.flow.lock(); channel && channel->_connection) return _forward_to(context, target, channel);

  // RFC 3261 16.6 step 7: open a flow to a hop this node has none to (a trunk, a peer node, or a client whose
  // connection has closed).
  const auto hop = _next_hop_of(*target.next_hop);

  // An IP literal needs no lookup. A name is resolved per RFC 3263 (NAPTR, SRV, then A and AAAA) into an
  // ordered list of hops.
  auto host = hop.host;
  if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);

  boost::system::error_code literal;
  boost::asio::ip::make_address(host, literal);
  if (!literal) return _connect_hops(context, target, {dns::Hop{hop.transport, host, hop.port}}, 0);

  auto self = shared_from_this();

  core->locator()->locate(core->strand(), *target.next_hop, [this, self, context, target](plugins::Result<std::vector<dns::Hop>> located) {
    if (!located.ok || located.value.empty()) {
      _logger->info("Nothing to send " + target.next_hop->to_string() + " to - " + (located.ok ? "DNS has no SIP service there" : located.error));
      return _unreachable(context);
    }

    _connect_hops(context, target, located.value, 0);
  });
}

// Tries each hop in turn until one connects.
void Proxy::_connect_hops(const std::shared_ptr<Context>& context, const Target& target, std::vector<dns::Hop> hops, std::size_t index) {
  auto core = _core.lock();
  if (!core) return;

  if (index >= hops.size()) return _unreachable(context);

  const auto hop = hops[index];
  auto self = shared_from_this();

  core->channel_connect(hop.transport, hop.address, hop.port,
                        [this, self, context, target, hops = std::move(hops), index, hop](plugins::Result<std::shared_ptr<Channel>> opened) mutable {
                          if (!opened.ok || !opened.value || !opened.value->_connection) {
                            _logger->info("No flow to " + target.next_hop->to_string() + " at " + hop.transport + "://" + hop.address + ":" +
                                          std::to_string(hop.port) + " - " + opened.error);
                            return _connect_hops(context, target, std::move(hops), index + 1);
                          }

                          context->hops_left.assign(hops.begin() + static_cast<std::ptrdiff_t>(index) + 1, hops.end());
                          context->hop_target = target;
                          _forward_to(context, target, opened.value);
                        });
}

// RFC 3263 4.3: retries the current target at the next DNS hop. False when there is none.
bool Proxy::_try_next_hop(const std::shared_ptr<Context>& context) {
  if (context->hops_left.empty() || !context->hop_target || context->cancelled || context->answered) return false;

  auto hops = std::move(context->hops_left);
  context->hops_left.clear();

  _logger->info("Trying " + context->hop_target->next_hop->to_string() + " at the next place DNS listed");
  _connect_hops(context, *context->hop_target, std::move(hops), 0);
  return true;
}

// Records a 480 for this target and moves on to the next (16.7).
void Proxy::_unreachable(const std::shared_ptr<Context>& context) {
  auto unavailable = context->request->generate_response();
  unavailable->header->response_code = 480;
  unavailable->header->response_message = "Temporarily Unavailable";

  if (!context->best) context->best = unavailable;
  _forward_next(context);
}

void Proxy::_forward_to(const std::shared_ptr<Context>& context, const Target& target, const std::shared_ptr<Channel>& channel) {
  // RFC 3261 16.6 step 1: every branch starts from a copy of the request as received.
  auto copy = context->request->clone();

  // Max-Forwards is the only failure, and it holds for every target, so the search ends (16.3).
  if (!_prepare_forward(copy, channel, target, context->loop_token)) {
    _logger->info("Max-Forwards exhausted - 483");
    return _send_status(context->server, context->request, 483, "Too Many Hops");
  }

  if (context->rewrite_contact) _rewrite_contact(copy, context->request->channel.lock());

  // The media engine handles the body first; the send follows asynchronously.
  auto self = shared_from_this();
  _anchor_media(context->request, copy, channel, context, [this, self, context, copy, channel]() { _send_forward(context, copy, channel); });
}

void Proxy::_send_forward(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& copy, const std::shared_ptr<Channel>& channel) {
  auto core = _core.lock();
  if (!core) return;

  // A CANCEL may have arrived while the media engine was working.
  if (context->answered || context->cancelled) return;

  const bool over_udp = channel->_connection && Util::to_lower(channel->_connection->transport_name()) == "udp";

  // RFC 3261 18.1.1: an oversized request goes over TCP instead, and the top Via is updated to match.
  if (over_udp && wire_size(copy) > kMaxUdpRequest) {
    // Same address and port: a symmetric UDP endpoint listens where it sends from.
    const auto hop = channel->_connection->remote_endpoint();

    auto self = shared_from_this();

    core->channel_connect("tcp", hop.address().to_string(), hop.port(), [this, self, context, copy, channel](plugins::Result<std::shared_ptr<Channel>> opened) {
      if (opened.ok && opened.value && opened.value->_connection) {
        set_top_via_transport(copy, "TCP");
        return _write_forward(context, copy, opened.value);
      }

      // 18.1.1: if TCP cannot be opened, fall back to UDP.
      _logger->info("Cannot open TCP for a request of " + std::to_string(wire_size(copy)) + " bytes - " + opened.error + " - sending it over UDP");

      _write_forward(context, copy, channel);
    });

    return;
  }

  _write_forward(context, copy, channel);
}

void Proxy::_write_forward(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& copy, const std::shared_ptr<Channel>& channel) {
  auto core = _core.lock();
  if (!core) return;

  if (context->answered || context->cancelled) return;

  // An ACK for a 2xx is outside any transaction (RFC 3261 17.1.1.3).
  if (!context->server) {
    channel->send(copy);
    return;
  }

  context->forwarded = copy;
  context->forwarded_flow = channel;
  context->provisional = false;
  context->timer_c_cancelled = false;

  // Drop entries whose context has gone, so the map does not grow for the life of the node.
  std::erase_if(_contexts, [](const auto& entry) { return entry.second.expired(); });

  _contexts[context->server->id()] = context;

  auto self = shared_from_this();

  context->client = core->client_transaction_start(
      copy, channel, [this, self, context](std::shared_ptr<SIPMessage> response) { _on_response(context, response); },
      [this, self, context]() {
        // Timer B or F (RFC 3261 16.7): an unanswered branch counts as a 408 and the next target is tried.
        _logger->info("No response from target - trying the next");

        auto timeout = context->request->generate_response();
        timeout->header->response_code = 408;
        timeout->header->response_message = "Request Timeout";

        if (!context->best) context->best = timeout;

        _timer_c_cancel(context);
        context->forwarded = nullptr;
        if (_try_next_hop(context)) return;
        if (!context->current.instance.empty()) _try_other_flow(context);
        _forward_next(context);
      });

  // RFC 3261 16.6 step 11: timer C is set for each proxied INVITE.
  _timer_c_start(context);
}

void Proxy::_on_response(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& response) {
  if (!context->server) return;

  // RFC 3261 16.7 step 3: remove this node's Via.
  if (response->header->contains("Via")) {
    auto top = response->header->headers_map["Via"][0];
    response->header->remove_value("Via", [&top](std::shared_ptr<headers::Header> header) { return header == top; });
  }

  const int code = response->header->response_code;

  // RFC 4028 8.2 runs before the dialog tracker, which reads the session interval from this response.
  if (is_2xx(code)) _complete_session_timer(context, response);

  // Dialogs (section 12) are tracked from what passes, not routed on. A non-2xx final below 600 while the
  // fork goes on ends only that branch (16.7); _send_best reports the end of the attempt.
  const bool fork_goes_on = is_final(code) && !is_2xx(code) && code < 600 && !context->answered;

  if (auto core = _core.lock()) {
    if (fork_goes_on) {
      core->dialogs()->observe_branch_failure(context->request, response);
    } else {
      core->dialogs()->observe_response(context->request, response);
    }
  }

  if (!is_final(code)) {
    context->provisional = true;

    // 16.7 step 2: a 101-199 resets timer C. A 100 does not.
    if (code > 100) _timer_c_start(context);

    // 9.1: a held CANCEL is sent once the branch has answered provisionally.
    if (context->cancelled) return _cancel_branch(context);

    if (!context->answered) _forward_response(context, response);
    return;
  }

  _timer_c_cancel(context);
  context->forwarded = nullptr;
  context->client = nullptr;

  // The caller already has its final response. Only a retransmitted 2xx is passed on, as first sent (RFC 6026
  // 8.4).
  if (context->answered) {
    if (is_2xx(code) && context->answer_sent) context->server->send(context->answer_sent);
    return;
  }

  // A 2xx or a 6xx ends the search (16.7 step 5).
  if (is_2xx(code) || code >= 600) {
    if (is_2xx(code)) _report_reoffer(context, true);

    // Record the peer node holding the callee, when the answer came from one.
    if (is_2xx(code) && context->request->header->request_method == "INVITE") {
      const auto answered_over = response->channel.lock();
      auto core = _core.lock();
      auto call = core ? core->call_get(value_of(context->request, "Call-ID")) : nullptr;

      if (call && answered_over && !answered_over->peer_node().empty()) {
        if (const auto callee = call->participant_index(false)) call->participants[*callee].node_id = answered_over->peer_node();
      }
    }
    context->best = response;
    context->answered = true;
    _contexts.erase(context->server->id());
    _forward_response(context, response);
    return;
  }

  // Serial forking: keep the lowest code as the best response and try the next target.
  if (!context->best || code < context->best->header->response_code) context->best = response;

  // RFC 3263 4.3: a 503 is the server failing, so the target's next DNS hop is tried.
  if (code == 503 && _try_next_hop(context)) return;

  // RFC 5626 5.3: a 408 or a 430 is the flow failing, so the instance's next flow is tried. Any other answer
  // is the client's, and its other flows are dropped.
  if (!context->current.instance.empty()) {
    if (code == 408 || code == 430) {
      _try_other_flow(context);
    } else {
      context->other_flows.erase(context->current.instance);
    }
  }

  if (code == 488) {
    if (_reoffer(context)) return;
    _report_reoffer(context, false);
  }

  _forward_next(context);
}

// Builds the target set for an address of record (RFC 3261 16.5). RFC 5626 5.3: an instance's flows form one
// target, most recently registered first, with the rest held back in other_flows. An ordinary binding is a
// target of its own.
void Proxy::_add_targets(const std::shared_ptr<Context>& context, std::vector<types::Location> bindings) const {
  std::stable_sort(bindings.begin(), bindings.end(), [](const types::Location& a, const types::Location& b) {
    return a.registered_at != b.registered_at ? a.registered_at > b.registered_at : a.reg_id > b.reg_id;
  });

  auto core = _core.lock();

  // A request from a peer node is only for the flows held here; the peer forks to the rest. Forwarding those
  // as well would ring devices twice or loop.
  const auto arrived = context->request->channel.lock();
  const bool from_peer = arrived && !arrived->peer_node().empty();

  std::set<std::string> forwarded_to;

  for (const auto& binding : bindings) {
    if (!binding.contact) continue;

    // RFC 8599: any node can push, so a push binding is pushed from here wherever its flow was. A request from a
    // peer is for the flows held here and is not pushed again.
    if (core && binding.push && !from_peer) {
      if (const auto pn = push::notification_of(*binding.contact); pn && core->push_service(pn->provider)) {
        auto target = _target_for(binding);
        target.push = binding;
        context->targets.push_back(std::move(target));
        continue;
      }
    }

    // Bindings are shared across the cluster; flows are not. A flow held by another node is reached through
    // that node, once per node.
    if (core && _held_elsewhere(*core, binding)) {
      if (from_peer) continue;

      if (auto peer = _peer_target(*core, binding.node_id, context->request)) {
        if (forwarded_to.insert(binding.node_id).second) context->targets.push_back(std::move(*peer));
        continue;
      }

      // The peer is down or unknown: fall through and treat the binding as a single node would.
    }

    auto target = _target_for(binding);
    if (target.instance.empty()) {
      context->targets.push_back(std::move(target));
      continue;
    }

    const bool first_of_instance = context->other_flows.find(target.instance) == context->other_flows.end();
    auto& others = context->other_flows[target.instance];

    if (first_of_instance) {
      context->targets.push_back(std::move(target));
    } else {
      others.push_back(std::move(target));
    }
  }
}

void Proxy::_push_and_wait(const std::shared_ptr<Context>& context, const Target& target) {
  auto core = _core.lock();
  if (!core || !target.push || !target.push->contact) return;

  auto notification = push::notification_of(*target.push->contact);
  auto service = notification ? core->push_service(notification->provider) : nullptr;
  if (!service) {
    auto plain = target;
    plain.push.reset();
    return _forward_target(context, plain);
  }

  // The bucket is found by a CANCEL like any branch in flight.
  context->pushed = target;
  context->current = target;
  _contexts[context->server->id()] = context;

  notification->reason = push::Notification::Reason::Request;
  _logger->info("Pushing to " + push::without_push_parameters(*target.push->contact).to_string() + " through " + notification->provider +
                " and holding the request");

  auto self = shared_from_this();
  service->send(core->strand(), *notification, [this, self, context](plugins::Status sent) {
    auto core = _core.lock();
    if (!core || context->answered || context->cancelled) return;

    if (!sent.ok) {
      _logger->info("The push to " + context->pushed->push->contact->to_string() + " failed - " + sent.error);
      context->pushed.reset();
      return _unreachable(context);
    }

    context->push_deadline = core->now() + std::chrono::seconds(core->config->push_timeout);
    _push_waiting.push_back(context);
    _push_poll(context);
  });
}

// The store is polled as well as told: in a cluster, the client may re-register through another node.
void Proxy::_push_poll(const std::shared_ptr<Context>& context) {
  auto core = _core.lock();
  if (!core) return;

  auto timers = core->timer_source();
  if (!timers) return;

  // The pending poll owns the context while the request is in the bucket: nothing else holds it until a branch
  // is forwarded. _push_check cancels it, which breaks the cycle.
  std::weak_ptr<TransactionUser> weak_self = weak_from_this();
  auto strand = core->strand();

  context->push_poll = timers->schedule(std::chrono::milliseconds(250), [weak_self, context, strand]() {
    boost::asio::post(strand, [weak_self, context]() {
      if (auto self = std::static_pointer_cast<Proxy>(weak_self.lock())) self->_push_check(context, "");
    });
  });
}

void Proxy::on_registered(std::uint64_t subscriber_id, const std::shared_ptr<SIPUri>& contact) {
  if (!contact) return;

  for (const auto& weak : std::vector<std::weak_ptr<Context>>(_push_waiting)) {
    auto context = weak.lock();
    if (context && context->pushed && context->pushed->push->subscriber_id == subscriber_id) _push_check(context, contact->to_string());
  }
}

// `registered` is a contact the registrar has just stored, which counts as refreshed whatever the store's
// one-second timestamps say.
void Proxy::_push_check(const std::shared_ptr<Context>& context, const std::string& registered) {
  auto core = _core.lock();
  if (!core || !context->pushed) return;

  auto finish = [this, context]() {
    if (context->push_poll) context->push_poll->cancel();
    context->push_poll.reset();
    context->pushed.reset();
    std::erase_if(_push_waiting, [&context](const std::weak_ptr<Context>& weak) {
      auto waiting = weak.lock();
      return !waiting || waiting == context;
    });
  };

  if (context->answered || context->cancelled) return finish();

  const auto pushed = *context->pushed->push;
  auto self = shared_from_this();

  core->location_list(pushed.subscriber_id, [this, self, context, pushed, registered, finish](plugins::Result<std::vector<types::Location>> found) {
    auto core = _core.lock();
    if (!core || !context->pushed || context->pushed->push->contact != pushed.contact) return;
    if (context->answered || context->cancelled) return finish();

    for (const auto& binding : found.ok ? found.value : std::vector<types::Location>{}) {
      if (!binding.contact) continue;

      // 5.3: the same push registration, or the same client instance if the service handed it a new prid.
      const bool same = push::same_push_parameters(*binding.contact, *pushed.contact) || (!pushed.instance.empty() && binding.instance == pushed.instance);
      const auto contact = binding.contact->to_string();
      const bool refreshed = contact == registered || contact != pushed.contact->to_string() || binding.registered_at != pushed.registered_at ||
                             binding.expires_at != pushed.expires_at;
      if (!same || !refreshed) continue;

      _logger->info(contact + " registered again - forwarding the request held for it");
      finish();

      std::optional<Target> woken;
      if (_held_elsewhere(*core, binding)) woken = _peer_target(*core, binding.node_id, context->request);
      if (!woken) woken = _target_for(binding);
      return _forward_target(context, *woken);
    }

    if (core->now() < context->push_deadline) return _push_poll(context);

    _logger->info(pushed.contact->to_string() + " did not register again within push.timeout - 480");
    finish();
    _unreachable(context);
  });
}

// A binding whose flow was learned by another node and is not open here.
bool Proxy::_held_elsewhere(Core& core, const types::Location& binding) const {
  if (binding.node_id.empty() || binding.node_id == core.config->sip_node_id || binding.flow_id.empty()) return false;

  return core.channel_find(binding.flow_id) == nullptr;
}

// The peer node's inter-node listener as a target for the request as it arrived. Nothing if the peer has not
// reported ok within three status intervals, or has no cluster address.
std::optional<Proxy::Target> Proxy::_peer_target(Core& core, const std::string& node_id, const std::shared_ptr<SIPMessage>& request) const {
  const auto interval = core.config->events_status_interval;
  const auto stale_after = interval == 0 ? std::chrono::seconds::max() : std::chrono::seconds(interval * 3);

  const auto node = core.nodes()->find(node_id, stale_after);
  if (!node || node->stale || node->status != "ok" || node->cluster_address.empty() || node->cluster_port == 0) return std::nullopt;

  auto hop = std::make_shared<SIPUri>();
  hop->valid = true;
  hop->scheme = "sip";
  hop->host = node->cluster_address;
  hop->port = node->cluster_port;
  hop->set_parameter("transport", "tls");

  Target target;
  target.uri = request->header->request_uri;
  target.next_hop = hop;
  target.flow = _flow_to(*hop);
  return target;
}

// Queues the current instance's next flow as the next target. False when it has none.
bool Proxy::_try_other_flow(const std::shared_ptr<Context>& context) {
  auto found = context->other_flows.find(context->current.instance);
  if (found == context->other_flows.end() || found->second.empty()) return false;

  auto next = std::move(found->second.front());
  found->second.erase(found->second.begin());

  context->targets.insert(context->targets.begin() + static_cast<std::ptrdiff_t>(context->next), std::move(next));
  return true;
}

// A 488 (RFC 3261 21.4.26) to an offer this node's engine produced for a callee that had not stated its
// profile earns one re-offer of the other profile, to the same target on a new transaction.
bool Proxy::_reoffer(const std::shared_ptr<Context>& context) {
  if (context->current.profile || !context->offered) return false;

  std::optional<media::Profile> other;
  if (*context->offered == media::Profile::PlainRtp) other = media::Profile::WebRtc;
  if (*context->offered == media::Profile::WebRtc) other = media::Profile::PlainRtp;
  if (!other) return false;

  // The engine must be able to produce the other profile.
  auto core = _core.lock();
  if (!core || !core->media || !core->media->produces(*other)) return false;

  Target again = context->current;
  again.profile = other;
  again.rejected = context->offered;
  context->targets.insert(context->targets.begin() + static_cast<std::ptrdiff_t>(context->next), again);

  _logger->info(context->request->header->request_uri->to_string() + " refused the offer it was made - offering the other profile");
  _forward_next(context);
  return true;
}

// Records the outcome of a re-offer for the operator (/api/v1/media/reoffers). The node does not change the
// subscriber's profile itself.
void Proxy::_report_reoffer(const std::shared_ptr<Context>& context, bool took) {
  const auto& current = context->current;
  if (!current.profile || !current.rejected) return;

  auto core = _core.lock();
  if (!core) return;

  const auto subscriber = context->request->header->request_uri->to_string();
  core->reoffers().record(subscriber, *current.rejected, took ? current.profile : std::nullopt);

  if (took) _logger->warn(subscriber + " took the other media profile after refusing the first - see /api/v1/media/reoffers");
}

void Proxy::_send_best(const std::shared_ptr<Context>& context) {
  if (!context->server || context->answered) return;

  _contexts.erase(context->server->id());

  if (!context->best) return _send_status(context->server, context->request, 480, "Temporarily Unavailable");

  // RFC 5626 section 11: 430 Flow Failed is between proxies; the caller gets a 480.
  if (context->best->header->response_code == 430) {
    context->answered = true;
    return _send_status(context->server, context->request, 480, "Temporarily Unavailable");
  }

  // RFC 3261 16.7 step 6: a 503 is about the next hop, not this node, so it goes upstream as a 500.
  if (context->best->header->response_code == 503) {
    context->answered = true;
    return _send_status(context->server, context->request, 500, "Server Internal Error");
  }

  // Also covers responses this node generated, such as a timer B 408. Observing a response twice is harmless.
  if (auto core = _core.lock()) core->dialogs()->observe_response(context->request, context->best);

  context->answered = true;
  _forward_response(context, context->best);
}

void Proxy::_forward_response(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& response) {
  if (context->rewrite_contact) _rewrite_contact(response, response->channel.lock());

  auto self = shared_from_this();
  auto server = context->server;

  // The response returns down the flow the request arrived on, which is the outgoing leg for media.
  _anchor_media(context->request, response, context->request->channel.lock(), context, [self, context, server, response]() {
    if (response->header->response_code >= 200 && response->header->response_code < 300) context->answer_sent = response;
    server->send(response);
  });
}

void Proxy::_anchor_media(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<SIPMessage>& message, const std::shared_ptr<Channel>& outgoing,
                          const std::shared_ptr<Context>& context, std::function<void()> then) {
  auto core = _core.lock();
  if (!core || !core->media || !has_sdp(message)) return then();

  auto call = core->call_get(value_of(request, "Call-ID"));
  auto dialog = core->dialogs()->find(request);

  // The engine must be told which end sent the description, which takes the call and the dialog.
  if (!call || !dialog) return then();

  // A call forwarded by a peer node is anchored on that node for its whole life. Decided by the initial
  // request and kept on the call.
  if (!request->in_known_dialog && tag_of(request, "To").empty()) {
    const auto arrived = request->channel.lock();
    if (arrived && !arrived->peer_node().empty()) call->media_elsewhere = true;
  }
  if (call->media_elsewhere) return then();

  // The realm's policy is kept on the call, because in-dialog requests look no realm up.
  if (context && context->media_policy) call->media_policy = *context->media_policy;

  // Likewise each end's subscriber profile, kept on its leg.
  if (context && context->callee_profile) {
    if (const auto callee = call->participant_index(false)) call->participants[*callee].subscriber_profile = context->callee_profile;
  }
  if (context && context->caller_profile) {
    if (const auto caller = call->participant_index(true)) call->participants[*caller].subscriber_profile = context->caller_profile;
  }

  const auto& policy = call->media_policy;

  // A realm that does not anchor media gets plain proxy behaviour (RFC 3261 16.6): the SDP is untouched.
  if (!policy.anchor) return then();

  const bool is_response = message->header->type == SIPHeader::Type::Response;
  const bool request_from_caller = dialog->is_from_caller(tag_of(request, "From"));

  // A response carries the description of the end the request did not come from.
  const bool from_caller = is_response ? !request_from_caller : request_from_caller;

  const auto participant = call->participant_index(from_caller);
  if (!participant) return then();

  // The description is produced for the other leg.
  const auto recipient = call->participant_index(!from_caller);

  // RFC 3264 section 5: the INVITE offers and its response answers. With no SDP in the INVITE, the response
  // is the offer and the ACK the answer.
  const bool is_offer = is_response ? !has_sdp(request) : message->header->request_method != "ACK";

  auto flags = media::Flags::from_sdp(message->body);
  flags.participant = *participant;

  // Record what this leg stated (ICE, DTLS, SRTP) before the engine rewrites the body; later descriptions for
  // this leg are produced from it.
  if (const auto stated = flags.stated()) call->participants[*participant].profile = *stated;

  // The profile to produce for the far leg, in order of precedence: a re-offer after a 488, what the leg has
  // stated, what it said when qualified, its subscriber's setting, then the realm's setting or the outgoing
  // transport.
  const bool unheard = !(recipient && call->participants[*recipient].profile);

  if (!is_response && context && context->current.profile) {
    flags.target = *context->current.profile;
  } else if (!unheard) {
    flags.target = *call->participants[*recipient].profile;
  } else if (!is_response && context && context->current.said) {
    flags.target = *context->current.said;
  } else if (recipient && call->participants[*recipient].subscriber_profile) {
    types::MediaPolicy subscriber;
    subscriber.profiles = *call->participants[*recipient].subscriber_profile;
    flags.target = _profile_under(subscriber, outgoing && outgoing->_connection ? outgoing->_connection->transport_name() : std::string());
  } else {
    flags.target = _profile_under(policy, outgoing && outgoing->_connection ? outgoing->_connection->transport_name() : std::string());
  }

  // A leg inside sip.localnet reaches the relay at the local address; others use the engine's public one.
  if (outgoing && outgoing->_connection && !core->config->public_address().empty() &&
      core->config->in_localnet(outgoing->_connection->remote_endpoint().address())) {
    flags.address = core->advertised_for(*outgoing).host;
  }

  // The profile offered to a callee that has not stated one, so that a 488 can be answered with the other.
  // Mirror resolves to what the caller stated.
  std::optional<media::Profile> offered;
  if (!is_response && is_offer && unheard && context && message->header->request_method == "INVITE") {
    offered = flags.target == media::Profile::Mirror ? flags.stated() : std::optional<media::Profile>(flags.target);

    // An engine that cannot produce the profile sends what it can, and there is no other profile to re-offer.
    // Warn only when the offer would have needed converting, naming the subscriber (the original Request-URI)
    // rather than the device's Contact.
    if (offered && !core->media->produces(*offered)) {
      if (flags.stated() != offered) {
        _logger->warn("The media engine cannot produce " + std::string(media::setting_name(*offered)) + " for " +
                      context->request->header->request_uri->to_string() + " - offering what it can");
      }
      offered.reset();
    }
  }

  auto self = shared_from_this();

  auto handler = [this, self, message, context, offered, then = std::move(then)](media::Result result) {
    if (result.ok) {
      // Only an offer the engine produced counts as this node's for a re-offer.
      if (context && offered) context->offered = offered;

      // Record which engine carried the call.
      if (auto core = _core.lock(); core && core->media) {
        if (auto anchored = core->call_get(value_of(message, "Call-ID"))) anchored->media_engine = core->media->describe();
      }

      message->body = std::move(result.sdp);
      message->body_length = static_cast<unsigned int>(message->body.size());
    } else {
      // The engine declined (a WebRTC offer at a plain-RTP relay, or no ports left). Forward the SDP
      // unchanged rather than fail the call.
      _logger->warn("Media engine declined the session description, passing it through - " + result.error);
    }

    then();
  };

  if (is_offer) {
    core->media->offer(core->strand(), call, message->body, flags, std::move(handler));
  } else {
    core->media->answer(core->strand(), call, message->body, flags, std::move(handler));
  }
}

bool Proxy::_prepare_forward(const std::shared_ptr<SIPMessage>& copy, const std::shared_ptr<Channel>& channel, const Target& target,
                             const std::string& loop_token) const {
  auto core = _core.lock();
  if (!core) return false;

  auto& header = copy->header;

  // RFC 3261 16.6 step 2: the Request-URI becomes the target.
  if (target.uri) header->request_uri = target.uri;

  // Step 3: decrement Max-Forwards. At zero the request cannot be forwarded (16.3 answers 483).
  if (header->contains("Max-Forwards")) {
    auto max_forwards = header->headers_map["Max-Forwards"][0]->as<UIntHeader>();
    if (max_forwards != nullptr) {
      if (max_forwards->value == 0) return false;
      max_forwards->value--;
    }
  } else {
    header->add("Max-Forwards", std::make_shared<UIntHeader>(kDefaultMaxForwards));
  }

  const auto transport = Util::to_lower(channel->_connection->transport_name());

  // Step 4: Record-Route on INVITE, so the rest of the dialog returns through this node, which media
  // anchoring and call records need.
  //
  // Always two values (RFC 5658 section 3): the top names the interface the request leaves on, the second the
  // one it arrived on. Each carries the flow token of the side it faces, so in-dialog requests go down a flow
  // rather than to a Contact, which for a browser is unreachable.
  if (header->request_method == "INVITE") {
    const bool secure_request = Util::to_lower(header->request_uri->scheme) == "sips";
    auto inbound = copy->channel.lock();

    // Names this node as the end it faces sees it (sip.localnet, public ports).
    // add_start prepends, so the bottom value is added first.
    if (inbound && inbound->_connection) {
      header->add_start("Record-Route",
                        _route_to_this_node(*core, Util::to_lower(inbound->_connection->transport_name()), *inbound, inbound->flow_token(), secure_request));
    }

    header->add_start("Record-Route", _route_to_this_node(*core, transport, *channel, channel->flow_token(), secure_request));
  }

  // RFC 3327 5.2: a forwarded REGISTER puts this node on the Path, if the client supports it, so the far
  // registrar's requests come back this way. RFC 5626 5.1: the first hop carries the flow token and "ob"; an
  // outbound client gets the Path whatever it said.
  if (header->request_method == "REGISTER") {
    auto inbound = copy->channel.lock();
    const bool outbound = header->contains("Contact") && header->headers_map["Contact"][0]->to_string().find("reg-id") != std::string::npos;

    if (inbound && inbound->_connection && (has_option_tag(copy, "Supported", "path") || outbound)) {
      const bool first_hop = header->headers_map["Via"].size() <= 1;
      auto path = _route_to_this_node(*core, Util::to_lower(inbound->_connection->transport_name()), *inbound, first_hop ? inbound->flow_token() : "",
                                      Util::to_lower(header->request_uri->scheme) == "sips");
      if (first_hop) path->value->uri->set_parameter("ob", "");
      header->add_start("Path", path);
    }
  }

  // Step 6: a top Route without lr is a strict router. It becomes the Request-URI, and the old Request-URI
  // goes to the end of the route set.
  if (header->contains("Route")) {
    auto top = header->headers_map["Route"][0];
    auto uri = route_uri(top);

    if (uri && !uri->has_parameter("lr")) {
      auto tail = std::make_shared<SIPIdentity>();
      tail->wrapped = true;
      tail->uri = header->request_uri;

      header->add("Route", std::make_shared<SIPIdentityHeader>(tail));
      header->request_uri = uri;
      header->remove_value("Route", [&top](std::shared_ptr<headers::Header> value) { return value == top; });
    }
  }

  // Step 8: this node's Via. The branch carries the loop token and a unique part, so each branch of a fork is
  // its own transaction.
  const auto branch = std::string(kMagicCookie) + loop_token + "." + Util::generate_random_string("", 12);

  const auto advertised = core->advertised_for(*channel);
  auto via =
      std::make_shared<ViaHeader>("SIP/2.0/" + Util::to_upper(transport) + " " + advertised.host + ":" + std::to_string(advertised.port) + ";branch=" + branch);

  header->add_start("Via", via);
  copy->branch = branch;

  return true;
}

void Proxy::on_cancel(std::shared_ptr<SIPMessage> cancel, std::shared_ptr<transactions::TransactionBase> cancel_transaction,
                      std::shared_ptr<transactions::TransactionBase> invite_transaction) {
  // RFC 3261 9.2: the CANCEL is answered here only when this node holds the INVITE's transaction. Otherwise
  // it is forwarded, and answering as well would give the caller two responses.
  if (cancel_transaction && invite_transaction) {
    auto ok = cancel->generate_response();
    ok->header->response_code = 200;
    ok->header->response_message = "OK";
    cancel_transaction->send(ok);
  }

  if (!invite_transaction) return _forward_cancel_statelessly(cancel, cancel_transaction);

  // RFC 3261 16.10: cancel the branch in flight.
  auto search = _contexts.find(invite_transaction->id());

  if (search != _contexts.end()) {
    if (auto context = search->second.lock()) {
      context->cancelled = true;
      context->answered = true;
      _cancel_branch(context);
    }

    _contexts.erase(search);
  }

  // The cancelled INVITE is answered 487.
  auto terminated = cancel->generate_response();
  terminated->header->response_code = 487;
  terminated->header->response_message = "Request Terminated";

  invite_transaction->send(terminated);
}

bool Proxy::_apply_session_timer(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction) {
  auto core = _core.lock();
  if (!core) return true;

  if (!is_session_refresh(request->header->request_method)) return true;

  auto* session = session_field_of(request, "Session-Expires");

  // 8.1: a request with no interval gets this node's.
  if (session == nullptr || session->delta_seconds == 0) {
    _insert_session_timer(request);
    return true;
  }

  const std::uint32_t minimum = core->config->sip_session_min_se;
  if (minimum == 0 || session->delta_seconds >= minimum) return true;

  // 8.1: a caller that supports "timer" is refused with a 422 and retries with a longer interval.
  if (has_option_tag(request, "Supported", "timer")) {
    _logger->info("Session-Expires of " + std::to_string(session->delta_seconds) + "s is below this node's minimum - 422");
    _send_interval_too_small(transaction, request, minimum);
    return false;
  }

  // 8.1: a caller that does not cannot act on a 422, so the interval is raised instead and Min-SE says what
  // the floor was. An existing Min-SE is raised, never lowered.
  std::uint32_t floor = minimum;

  if (auto* min_se = session_field_of(request, "Min-SE"); min_se != nullptr) {
    if (min_se->delta_seconds < minimum) min_se->delta_seconds = minimum;
    floor = min_se->delta_seconds;
  } else {
    request->header->add("Min-SE", std::make_shared<headers::SessionExpiresHeader>(minimum));
  }

  // 8.1: Session-Expires is raised to the Min-SE. The refresher parameter is not the proxy's to change.
  _logger->info("Raising a Session-Expires of " + std::to_string(session->delta_seconds) + "s to " + std::to_string(floor) +
                "s - the caller does not support timer");

  session->delta_seconds = floor;
  return true;
}

void Proxy::_insert_session_timer(const std::shared_ptr<SIPMessage>& request) {
  auto core = _core.lock();
  if (!core) return;

  // Disabled, or a Session-Expires is already present. A zero value is malformed and is left alone.
  if (core->config->sip_session_expires == 0) return;
  if (session_field_of(request, "Session-Expires") != nullptr) return;

  // 8.1: the inserted interval is not below the request's Min-SE.
  std::uint32_t interval = core->config->sip_session_expires;
  if (auto* min_se = session_field_of(request, "Min-SE"); min_se != nullptr && min_se->delta_seconds > interval) interval = min_se->delta_seconds;

  // 8.1: no refresher parameter; the endpoints settle that in the 2xx (section 9).
  request->header->add("Session-Expires", std::make_shared<headers::SessionExpiresHeader>(interval));

  _logger->debug("Call asked for no session interval - offering " + std::to_string(interval) + "s");

  // 8.1: inserting Require: timer is permitted but NOT RECOMMENDED, because a callee without RFC 4028 answers
  // 420 and the call fails. Done only when sip_require_session_timer is set.
  if (!core->config->sip_require_session_timer) return;
  if (has_option_tag(request, "Supported", "timer")) return;
  if (has_option_tag(request, "Require", "timer")) return;

  _logger->debug("Requiring the session timer this node offered");
  request->header->add("Require", "timer");
}

void Proxy::_complete_session_timer(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& response) {
  if (!is_session_refresh(context->request->header->request_method)) return;

  // RFC 4028 8.2: a response with Session-Expires needs no change.
  if (session_field_of(response, "Session-Expires") != nullptr) return;

  // Neither end asked for a session timer, so none is added.
  if (!context->session_timer_supported || context->session_interval == 0) return;

  // 8.2: this is the first timer-aware proxy on the response path, so it inserts the interval it forwarded.
  auto session = std::make_shared<headers::SessionExpiresHeader>(context->session_interval);

  // 8.2: the refresher is "uac"; the callee does not know the session has a timer.
  session->refresher = "uac";
  response->header->add("Session-Expires", session);

  // 8.2: and "timer" is required.
  if (!has_option_tag(response, "Require", "timer")) response->header->add("Require", "timer");

  _logger->info("Callee answered without a session timer - telling the caller its own interval stands, refreshed by it");
}

void Proxy::_send_interval_too_small(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                                     std::uint32_t minimum) {
  if (!transaction) return;

  auto response = request->generate_response();
  response->header->response_code = 422;
  response->header->response_message = "Session Interval Too Small";

  // RFC 4028 section 6: a 422 must carry Min-SE.
  response->header->add("Min-SE", std::make_shared<headers::SessionExpiresHeader>(minimum));

  if (auto core = _core.lock()) core->dialogs()->observe_response(request, response);

  transaction->send(response);
}

void Proxy::_timer_c_start(const std::shared_ptr<Context>& context) {
  _timer_c_cancel(context);

  auto core = _core.lock();

  // Tied to the client transaction (16.6 step 11), not to `forwarded`, which _cancel_branch clears while the
  // branch is still outstanding.
  if (!core || !context->client) return;

  // INVITE only: other methods are bounded by timer F (RFC 3261 17.1.2.2).
  if (context->request->header->request_method != "INVITE") return;

  auto timers = core->timer_source();
  if (!timers) return;

  // Weak, so a pending timer keeps neither the proxy nor the context alive.
  std::weak_ptr<TransactionUser> weak_self = weak_from_this();
  std::weak_ptr<Context> weak_context = context;

  context->timer_c = timers->schedule(std::chrono::milliseconds(core->config->sip_timer_c_invite_proxy_ms), [weak_self, weak_context]() {
    auto self = std::static_pointer_cast<Proxy>(weak_self.lock());
    auto held = weak_context.lock();

    if (self && held) self->_on_timer_c(held);
  });
}

void Proxy::_timer_c_cancel(const std::shared_ptr<Context>& context) {
  if (!context->timer_c) return;

  context->timer_c->cancel();
  context->timer_c = nullptr;
}

void Proxy::_on_timer_c(const std::shared_ptr<Context>& context) {
  context->timer_c = nullptr;

  // RFC 3261 16.8: a branch that has answered provisionally is sent a CANCEL. The timer is reset once, so the
  // branch can finish with a 487 through _on_response.
  if (context->provisional && !context->timer_c_cancelled) {
    _logger->info("Timer C fired on a branch that is still provisional - cancelling it");

    context->timer_c_cancelled = true;
    _cancel_branch(context);
    _timer_c_start(context);
    return;
  }

  // The branch never answered, or ignored the CANCEL: terminate it and treat it as a 408 (16.8).
  _logger->info("Timer C fired on a branch that will not finish - giving up on it");

  if (context->client) {
    context->client->terminate();
    context->client = nullptr;
  }

  context->forwarded = nullptr;

  // The caller already has its answer.
  if (context->answered) return;

  if (!context->best) {
    auto timeout = context->request->generate_response();
    timeout->header->response_code = 408;
    timeout->header->response_message = "Request Timeout";
    context->best = timeout;
  }

  _forward_next(context);
}

void Proxy::_cancel_branch(const std::shared_ptr<Context>& context) {
  auto core = _core.lock();
  if (!core) return;

  // RFC 3261 9.1: no CANCEL before a provisional response; it waits for one.
  if (!context->provisional || !context->forwarded) return;

  auto channel = context->forwarded_flow.lock();
  if (!channel) return;

  const auto& forwarded = context->forwarded;

  // 9.1: the CANCEL is the forwarded request with the method changed, only this node's Via, the same Route
  // set and no body.
  auto cancel = forwarded->clone();

  cancel->header->request_method = "CANCEL";
  cancel->body.clear();
  cancel->body_length = 0;

  cancel->header->clear("Record-Route");
  cancel->header->clear("Contact");
  cancel->header->clear("Content-Type");

  if (cancel->header->contains("Via")) {
    auto top = cancel->header->headers_map["Via"][0];
    cancel->header->remove_value("Via", [&top](std::shared_ptr<headers::Header> value) { return value != top; });
  }

  if (cancel->header->contains("CSeq")) {
    auto cseq = cancel->header->headers_map["CSeq"][0]->as<CSeqHeader>();
    if (cseq != nullptr) cseq->method = "CANCEL";
  }

  // The CANCEL is its own transaction (9.1) sharing the INVITE's branch; the method keeps them distinct.
  core->client_transaction_start(cancel, channel, nullptr, nullptr);

  context->forwarded = nullptr;
}

// RFC 3261 16.10: a CANCEL with no response context is forwarded statelessly, as the request it names may
// have been.
void Proxy::_forward_cancel_statelessly(const std::shared_ptr<SIPMessage>& cancel, const std::shared_ptr<transactions::TransactionBase>& transaction) {
  auto core = _core.lock();
  if (!core) return;

  // 16.4, as for any request; the last Route removed names the flow to use.
  _preprocess_routes(cancel);

  std::shared_ptr<SIPUri> next_hop;
  if (cancel->header->contains("Route")) next_hop = route_uri(cancel->header->headers_map["Route"][0]);
  if (!next_hop) next_hop = cancel->header->request_uri;

  auto flow = next_hop ? _flow_to(*next_hop) : nullptr;
  if (!flow) flow = core->channel_for_token(cancel->flow_token);

  if (!flow) {
    // Nowhere to send it: 481 (RFC 3261 9.2).
    _logger->info("CANCEL with no response context and nowhere to forward it - 481");
    return _send_status(transaction, cancel, 481, "Call/Transaction Does Not Exist");
  }

  // 16.11: a stateless branch is computed from the request, so a retransmitted CANCEL is the same
  // transaction.
  const auto branch = std::string(kMagicCookie) + _loop_token(cancel);

  const auto transport = Util::to_lower(flow->_connection->transport_name());

  if (cancel->header->contains("Max-Forwards")) {
    auto max_forwards = cancel->header->headers_map["Max-Forwards"][0]->as<UIntHeader>();

    if (max_forwards != nullptr) {
      if (max_forwards->value == 0) {
        _logger->info("CANCEL has run out of hops - 483");
        return _send_status(transaction, cancel, 483, "Too Many Hops");
      }

      max_forwards->value--;
    }
  } else {
    cancel->header->add("Max-Forwards", std::make_shared<UIntHeader>(kDefaultMaxForwards));
  }

  const auto advertised = core->advertised_for(*flow);
  auto via =
      std::make_shared<ViaHeader>("SIP/2.0/" + Util::to_upper(transport) + " " + advertised.host + ":" + std::to_string(advertised.port) + ";branch=" + branch);

  cancel->header->add_start("Via", via);
  cancel->branch = branch;

  _logger->info("CANCEL with no response context, forwarded statelessly to " + flow->flow_id());

  // The response matches no client transaction and returns through on_stray_response (16.7 step 1).
  flow->send(cancel);
}

void Proxy::on_stray_response(std::shared_ptr<SIPMessage> response) {
  auto core = _core.lock();
  if (!core) return;

  if (!response->header->contains("Via")) {
    _logger->debug("Response " + std::to_string(response->header->response_code) + " with no Via - dropping");
    return;
  }

  auto top = response->header->headers_map["Via"][0];
  auto top_via = top->as<ViaHeader>();

  // RFC 3261 18.1.2: only a response whose top Via is this node's is forwarded.
  if (top_via == nullptr) return;

  const auto [top_host, top_port] = split_sent_by(top_via->host, 5060);

  if (!core->is_local_address(top_host, top_port)) {
    _logger->debug("Response " + std::to_string(response->header->response_code) + " whose top Via is not this node - dropping");
    return;
  }

  response->header->remove_value("Via", [&top](std::shared_ptr<headers::Header> header) { return header == top; });

  if (!response->header->contains("Via")) {
    _logger->debug("Response " + std::to_string(response->header->response_code) + " for this node with no transaction - dropping");
    return;
  }

  auto next_via = response->header->headers_map["Via"][0]->as<ViaHeader>();
  if (next_via == nullptr) return;

  // 18.2.2: send to received and rport when present, otherwise to sent-by.
  const auto transport = transport_of_via(*next_via);
  auto [host, port] = split_sent_by(next_via->host, transport == "tls" ? 5061 : 5060);

  const auto received = next_via->parameters.find("received");
  if (received != next_via->parameters.end() && !received->second.empty()) host = received->second;

  const auto rport = next_via->parameters.find("rport");
  if (rport != next_via->parameters.end() && !rport->second.empty()) {
    try {
      const auto parsed = std::stoul(rport->second);
      if (parsed == 0 || parsed > 65535) throw std::out_of_range("rport");
      port = static_cast<std::uint16_t>(parsed);
    } catch (const std::exception&) {
      _logger->debug("Response " + std::to_string(response->header->response_code) + " with an rport that is not a port - dropping");
      return;
    }
  }

  // A forgotten UDP flow is reopened from the listener's socket, and a closed connection is opened anew
  // (18.2.2). A response that cannot be delivered is dropped.
  const auto name = host + ":" + std::to_string(port);

  core->channel_connect(transport, host, port, [this, self = shared_from_this(), response, name](plugins::Result<std::shared_ptr<Channel>> opened) {
    if (!opened.ok || !opened.value) {
      _logger->info("No flow back to " + name + " for a stray response - dropping - " + opened.error);
      return;
    }

    opened.value->send(response);
  });
}

void Proxy::_answer_options(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request) {
  auto response = request->generate_response();
  response->header->response_code = 200;
  response->header->response_message = "OK";

  // 11.2: what this node does. As a proxy it carries any method; these are the ones it takes part in.
  response->header->add("Allow", std::make_shared<headers::StringHeader>("INVITE, ACK, CANCEL, BYE, OPTIONS, REGISTER, UPDATE, INFO, PRACK, MESSAGE, "
                                                                         "SUBSCRIBE, NOTIFY, REFER"));
  response->header->add("Accept", std::make_shared<headers::StringHeader>("application/sdp"));
  response->header->add("Accept-Encoding", std::make_shared<headers::StringHeader>("identity"));
  response->header->add("Accept-Language", std::make_shared<headers::StringHeader>("en"));
  response->header->add("Supported", std::make_shared<headers::StringHeader>("path, outbound, timer"));

  transaction->send(response);
}

void Proxy::forward_register(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction) {
  if (!request->header->request_uri) return _send_status(transaction, request, 400, "Bad Request");

  const auto token = _loop_token(request);
  if (_is_loop(request, token)) {
    _logger->info("REGISTER has been here before with nothing changed - 482");
    return _send_status(transaction, request, 482, "Loop Detected");
  }

  _preprocess_routes(request);

  auto self = shared_from_this();
  _authorize_relay(request, transaction, [this, self, request, transaction, token]() {
    _logger->info("Forwarding a REGISTER for " + request->header->request_uri->to_string() + " (RFC 3261 10.3 step 1)");
    _determine_targets(request, transaction, token);
  });
}

void Proxy::_authorize_relay(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction,
                             std::function<void()> then) {
  auto core = _core.lock();
  if (!core) return;

  // A reliable connection a REGISTER authenticated over is trusted, as for calls. A UDP source address is not.
  auto channel = request->channel.lock();
  if (channel && channel->_connection && channel->_connection->is_reliable() && channel->is_authenticated()) return then();

  // The credentials name their realm, which must be one of this node's.
  std::shared_ptr<headers::Header> answered;
  std::shared_ptr<types::Authorization> credentials;
  for (const auto& value : request->header->headers_map["Proxy-Authorization"]) {
    auto header = value->as<headers::AuthorizationHeader>();
    if (header == nullptr || !digest::is_complete(header->value) || !header->value->contains_field("username")) continue;
    answered = value;
    credentials = header->value;
    break;
  }

  if (!credentials) {
    _logger->info("REGISTER to forward from a sender with no credentials here - challenging");
    return _send_relay_challenge(transaction, request);
  }

  auto self = shared_from_this();
  const auto realm_name = Util::to_lower(credentials->fields["realm"]);

  core->realm_get_by_name(realm_name, [this, self, request, transaction, credentials, answered, then](plugins::Result<std::shared_ptr<types::Realm>> realm) {
    auto core = _core.lock();
    if (!core) return;

    if (!realm.ok) {
      _logger->error("Could not read a realm - " + realm.error);
      return _send_status(transaction, request, 500, "Server Internal Error");
    }
    if (!realm.value) return _send_relay_challenge(transaction, request);

    core->nonce_check(credentials->fields["nonce"], [this, self, request, transaction, credentials, answered, then,
                                                     realm = realm.value](plugins::Result<bool> checked) {
      auto core = _core.lock();
      if (!core) return;

      if (!checked.ok) {
        _logger->error("Could not check a nonce - " + checked.error);
        return _send_status(transaction, request, 500, "Server Internal Error");
      }
      if (!checked.value) return _send_relay_challenge(transaction, request);

      auto claimed = std::make_shared<SIPIdentity>("sip:" + credentials->fields["username"] + "@" + realm->name);
      core->subscriber_get(claimed, [this, self, request, transaction, credentials, answered, then](plugins::Result<std::shared_ptr<types::Subscriber>> found) {
        if (!found.ok) {
          _logger->error("Could not read a subscriber - " + found.error);
          return _send_status(transaction, request, 500, "Server Internal Error");
        }

        // An unknown subscriber is challenged like a wrong password, so subscribers cannot be enumerated.
        if (!found.value || !digest::verify(*found.value, *credentials, request->header->request_method).empty()) {
          _logger->info("REGISTER to forward with credentials that do not verify - challenging");
          return _send_relay_challenge(transaction, request);
        }

        // Spent here; the far registrar's Authorization is left alone (22.3).
        request->header->remove_value("Proxy-Authorization", [&answered](std::shared_ptr<headers::Header> value) { return value == answered; });
        then();
      });
    });
  });
}

// One challenge for each of this node's realms, each with its own nonce: the From is the foreign address of
// record, so nothing says which realm the sender belongs to (RFC 3261 22.3 allows several).
void Proxy::_send_relay_challenge(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request) {
  auto core = _core.lock();
  if (!core) return;

  auto self = shared_from_this();
  core->datastore->realm_list(core->strand(), [this, self, transaction, request](plugins::Result<std::vector<std::shared_ptr<types::Realm>>> realms) {
    if (!realms.ok || realms.value.empty()) {
      _logger->info("REGISTER to forward, and no realm here to authenticate it - 403");
      return _send_status(transaction, request, 403, "Forbidden");
    }

    auto response = request->generate_response();
    response->header->response_code = 407;
    response->header->response_message = "Proxy Authentication Required";

    auto pending = std::make_shared<std::size_t>(realms.value.size());
    for (const auto& realm : realms.value) {
      auto core = _core.lock();
      if (!core) return;

      // The nonce must be stored before it is sent, or it would fail its own check.
      core->nonce_create(realm, [this, self, transaction, request, response, realm, pending](plugins::Result<std::string> nonce) {
        if (nonce.ok) digest::add_challenges(*response->header, "Proxy-Authenticate", realm->name, nonce.value);
        if (--*pending == 0) transaction->send(response);
      });
    }
  });
}

std::shared_ptr<headers::SIPIdentityHeader> Proxy::_route_to_this_node(Core& core, const std::string& transport, const Channel& facing,
                                                                       const std::string& token, bool secure) const {
  const auto advertised = core.advertised_for(facing);
  auto uri = std::make_shared<SIPUri>();
  uri->valid = true;
  uri->scheme = (transport == "tls" || secure) ? "sips" : "sip";
  uri->host = advertised.host;
  uri->port = advertised.port;

  // RFC 5626 5.1: the flow token is the user part.
  uri->user = token;

  // 19.1.1: lr marks this node as a loose router.
  uri->set_parameter("lr", "");
  if (transport != "udp") uri->set_parameter("transport", transport);

  auto identity = std::make_shared<SIPIdentity>();
  identity->wrapped = true;
  identity->uri = uri;

  return std::make_shared<SIPIdentityHeader>(identity);
}

bool Proxy::_names_this_node(const SIPUri& uri) const {
  auto core = _core.lock();
  if (!core) return false;

  const auto hop = _next_hop_of(uri);
  return core->is_local_address(hop.host, hop.port);
}

// The policy's profile, with the transport deciding only for FromTransport. A realm can override the
// transport because SIP over WebSocket (RFC 7118) does not imply WebRTC.
media::Flags::Profile Proxy::_profile_under(const types::MediaPolicy& policy, const std::string& transport) {
  switch (policy.profiles) {
    case types::MediaPolicy::Profiles::Mirror:
      return media::Flags::Profile::Mirror;
    case types::MediaPolicy::Profiles::PlainRtp:
      return media::Flags::Profile::PlainRtp;
    case types::MediaPolicy::Profiles::WebRtc:
      return media::Flags::Profile::WebRtc;
    case types::MediaPolicy::Profiles::SrtpSdes:
      return media::Flags::Profile::SrtpSdes;
    case types::MediaPolicy::Profiles::FromTransport:
      break;
  }

  return media::Flags::profile_for_transport(transport);
}

Proxy::Target Proxy::_target_for(const types::Location& binding) const {
  // RFC 5626 5.3: the Request-URI is the registered Contact, however the request is sent.
  Target target;
  target.uri = binding.contact;
  target.next_hop = binding.contact;

  auto core = _core.lock();
  if (!core) return target;

  target.instance = binding.reg_id != 0 ? binding.instance : std::string();

  // The flow the binding registered over, if still open. It is the only way back to a browser or a NAT'd
  // client.
  if (auto flow = core->channel_find(binding.flow_id)) {
    target.flow = flow;
    target.said = core->qualifier()->said(binding.flow_id);
    return target;
  }

  // A forgotten UDP flow can still be sent down (RFC 5626 3.1), but only from the node that held it: the far
  // NAT knows no other source address.
  const bool held_here = binding.node_id.empty() || binding.node_id == core->config->sip_node_id;

  if (auto hop = held_here ? _datagram_hop(binding.flow_id) : nullptr) {
    target.next_hop = hop;
    target.flow = _flow_to(*hop);
    return target;
  }

  // RFC 5626 5.3: an outbound binding is reached only down its flow, so a closed flow fails the target.
  target.instance = binding.reg_id != 0 ? binding.instance : std::string();
  if (!target.instance.empty()) {
    target.dead = true;
    return target;
  }

  // No flow, or a closed reliable one: only the Contact is left. If the client has gone the attempt fails and
  // the fork moves on.
  target.flow = _flow_to(*binding.contact);
  return target;
}

// The next hop a UDP flow id names, for a flow with no channel. Null for other transports: a closed
// connection can only be reopened by its client.
std::shared_ptr<SIPUri> Proxy::_datagram_hop(const std::string& flow_id) {
  static const std::string kScheme = Core::channel_key("udp", "");

  if (flow_id.rfind(kScheme, 0) != 0) return nullptr;

  // host:port; the port follows the last colon, as the host may be IPv6.
  const auto endpoint = flow_id.substr(kScheme.size());
  const auto colon = endpoint.rfind(':');
  if (colon == std::string::npos || colon == 0) return nullptr;

  std::uint16_t port = 0;
  try {
    const auto parsed = std::stoul(endpoint.substr(colon + 1));
    if (parsed == 0 || parsed > 65535) return nullptr;
    port = static_cast<std::uint16_t>(parsed);
  } catch (const std::exception&) {
    return nullptr;
  }

  auto hop = std::make_shared<SIPUri>();
  hop->scheme = "sip";
  hop->host = endpoint.substr(0, colon);
  hop->port = port;
  hop->set_parameter("transport", "udp");

  return hop;
}

std::shared_ptr<Channel> Proxy::_flow_to(const SIPUri& uri) const {
  auto core = _core.lock();
  if (!core) return nullptr;

  const auto hop = _next_hop_of(uri);
  return core->channel_find(hop.transport, hop.host, hop.port);
}

// RFC 3261 16.6 step 7: transport, host and port from the URI alone, with defaults. RFC 3263 resolution
// happens in _forward_next.
Proxy::NextHop Proxy::_next_hop_of(const SIPUri& uri) {
  NextHop hop;

  const bool secure = Util::to_lower(uri.scheme) == "sips";

  hop.transport = Util::to_lower(uri.parameter("transport"));
  if (hop.transport.empty()) hop.transport = secure ? "tls" : "udp";

  // ws and wss share one transport; TLS is a property of the listener.
  if (hop.transport == "wss") hop.transport = "ws";

  hop.host = uri.host;
  hop.port = uri.port.value_or(hop.transport == "tls" ? 5061 : 5060);

  return hop;
}

void Proxy::_send_status(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request, std::uint16_t code,
                         const std::string& reason) {
  if (!transaction) return;

  auto response = request->generate_response();
  response->header->response_code = code;
  response->header->response_message = reason;

  // Tell the dialog tracker: a failure answered here ends the attempt.
  if (auto core = _core.lock()) core->dialogs()->observe_response(request, response);

  transaction->send(response);
}

}  // namespace athenasip
