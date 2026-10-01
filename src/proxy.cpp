//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "proxy.h"

#include <chrono>
#include <utility>

#include "call.h"
#include "channel.h"
#include "core.h"
#include "digest.h"
#include "headers/authorization_header.h"
#include "headers/cseq_header.h"
#include "headers/session_expires_header.h"
#include "headers/sip_identity_header.h"
#include "headers/uint_header.h"
#include "headers/via_header.h"
#include "loggers/logger_scoped.h"
#include "media/media_engine.h"
#include "types/location.h"
#include "util.h"

namespace athenasip {

using athenasip::headers::CSeqHeader;
using athenasip::headers::SIPIdentityHeader;
using athenasip::headers::UIntHeader;
using athenasip::headers::ViaHeader;

namespace {

constexpr std::uint64_t kDefaultMaxForwards = 70;

// The branch is "z9hG4bK" + the loop token + "." + a value unique to this branch
// (RFC 3261 8.1.1.7 and 16.6 step 8). The two halves are separable so that 16.3.4 can
// read the token back out of a Via this node wrote.
constexpr const char* kMagicCookie = "z9hG4bK";
constexpr std::size_t kLoopTokenLength = 16;

// RFC 3261 18.1.1: "if it is larger than 1300 bytes and the path MTU is unknown". This
// node does no path MTU discovery, so the MTU is always unknown and the number is the
// whole of the rule.
constexpr std::size_t kMaxUdpRequest = 1300;

bool is_2xx(int code) { return code >= 200 && code < 300; }
bool is_final(int code) { return code >= 200; }

// RFC 3261 20.42: sent-by = host [ COLON port ]. An IPv6 reference keeps its brackets,
// so the port is only the part after the last colon outside them.
std::pair<std::string, std::uint16_t> split_sent_by(const std::string& sent_by, std::uint16_t default_port) {
  const auto bracket = sent_by.rfind(']');
  const auto colon = sent_by.rfind(':');

  if (colon == std::string::npos || (bracket != std::string::npos && colon < bracket)) return {sent_by, default_port};

  const auto port = sent_by.substr(colon + 1);
  if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos) return {sent_by, default_port};

  return {sent_by.substr(0, colon), static_cast<std::uint16_t>(std::stoul(port))};
}

// The transport a Via names, as a flow is filed under it. "SIP/2.0/UDP" -> "udp".
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

// The SIPUri of a Route or Record-Route value, or null when the value did not parse as
// a name-addr.
std::shared_ptr<SIPUri> route_uri(const std::shared_ptr<headers::Header>& header) {
  auto identity = header->as<SIPIdentityHeader>();
  if (identity == nullptr || identity->value == nullptr) return nullptr;
  return identity->value->uri;
}

// RFC 3261 20.15: a body is a session description when the Content-Type says so. A body
// with no Content-Type at all is malformed and is not guessed at.
bool has_sdp(const std::shared_ptr<SIPMessage>& message) {
  if (message->body.empty() || !message->header->contains("Content-Type")) return false;
  return Util::to_lower(message->header->headers_map["Content-Type"][0]->to_string()).rfind("application/sdp", 0) == 0;
}

// The size of the message as it will actually leave. Content-Length is fixed up by the
// transport on the way out, so it is made right here too: measuring before that would be
// measuring a message that is not the one sent. Doing it twice costs nothing and gets the
// same answer.
std::size_t wire_size(const std::shared_ptr<SIPMessage>& message) {
  message->header->clear("Content-Length");
  message->header->add("Content-Length", std::make_shared<UIntHeader>(message->body.size()));
  return message->to_string().size();
}

// RFC 3261 20.42: sent-protocol is "SIP/2.0/<transport>". Changing where a request goes
// out means changing what the top Via says it went out over (18.1.1).
void set_top_via_transport(const std::shared_ptr<SIPMessage>& message, const std::string& transport) {
  if (!message->header->contains("Via")) return;

  auto via = message->header->headers_map["Via"][0]->as<ViaHeader>();
  if (via == nullptr) return;

  const auto slash = via->version.rfind('/');
  via->version = (slash == std::string::npos ? std::string("SIP/2.0") : via->version.substr(0, slash)) + "/" + transport;
}

// RFC 4028 sections 4 and 5: both fields are delta-seconds, and Min-SE shares the type
// because the only thing it does not carry is a refresher parameter.
headers::SessionExpiresHeader* session_field_of(const std::shared_ptr<SIPMessage>& message, const std::string& field) {
  if (!message->header->contains(field)) return nullptr;
  return message->header->headers_map[field][0]->as<headers::SessionExpiresHeader>();
}

// RFC 3261 20.37 and 20.32: Supported and Require are comma-separated option tags, which
// the header table already splits into one value each.
bool has_option_tag(const std::shared_ptr<SIPMessage>& message, const std::string& field, const std::string& tag) {
  if (!message->header->contains(field)) return false;

  for (const auto& value : message->header->headers_map[field]) {
    if (Util::to_lower(Util::trim(value->to_string())) == tag) return true;
  }

  return false;
}

// RFC 4028 section 4: a session interval is negotiated on an INVITE or an UPDATE and
// nowhere else.
bool is_session_refresh(const std::string& method) { return method == "INVITE" || method == "UPDATE"; }

// The loop half of a branch this node wrote, or empty for a branch it did not.
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

  // RFC 3261 16.3.4, on the request exactly as it arrived: the token has to be the same
  // one that went out, and route preprocessing is about to change what it is made of.
  const auto token = _loop_token(request);

  if (_is_loop(request, token)) {
    _logger->info("Request has been here before with nothing changed - 482");
    return _send_status(transaction, request, 482, "Loop Detected");
  }

  // RFC 4028 section 8.1, before any copy of the request goes anywhere: an interval this
  // node will not keep state for is either refused or raised, depending on whether the
  // caller knows what a session timer is.
  if (!_apply_session_timer(request, transaction)) return;

  // RFC 3261 16.4, then who may send this where, then 16.5. Routes first, because a route
  // set naming somewhere else is a request leaving this node whatever its Request-URI says.
  _preprocess_routes(request);

  auto self = shared_from_this();
  _authorize(request, transaction, [this, self, request, transaction, token]() { _determine_targets(request, transaction, token); });
}

// The 2026-10-01 decision in TODO/ACTIVE.md: this node is not an open relay. A caller
// claiming to be one of this node's subscribers proves it, whoever it is calling; a caller
// from anywhere else may call into this node's domains and nowhere else.
void Proxy::_authorize(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction,
                       std::function<void()> then) {
  auto core = _core.lock();
  if (!core) return;

  // RFC 3261 22.1: a CANCEL cannot be challenged, and nor can the ACK - neither has a
  // response the caller could resubmit through.
  const auto& method = request->header->request_method;
  if (method == "ACK" || method == "CANCEL") return then();

  // Part of a call that was let through when it was made. The dialog table says so, not
  // the To tag: a tag is anybody's to write, and would otherwise be the way round all of
  // this.
  if (!tag_of(request, "To").empty() && request->in_known_dialog) return then();

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

    // A stranger. A route set still to follow leads off this node, so that is no.
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

      // Receiving a call, which anybody may make.
      if (target.value) return then();

      _logger->info("Request from " + caller->to_string() + " to " + request->header->request_uri->to_string() + ", neither of them here - 403");
      _send_status(transaction, request, 403, "Forbidden");
    });
  });
}

// RFC 3261 22.3, for a caller whose From is in a realm this node serves.
void Proxy::_authenticate(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction,
                          const std::shared_ptr<types::Realm>& realm, const std::shared_ptr<SIPUri>& caller, std::function<void()> then) {
  auto core = _core.lock();
  if (!core) return;

  // A connection a REGISTER was authenticated over belongs to that subscriber. A UDP
  // source address does not belong to anybody.
  auto channel = request->channel.lock();
  if (channel && channel->_connection && channel->_connection->is_reliable() && channel->is_authenticated_as(caller->to_string())) return then();

  // The credentials for this realm. Others, for proxies further on, are not this node's to
  // read or to remove (22.3).
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

        // The credentials prove whoever they name, and that is checked first; whether that is
        // who the From says comes after, so the two failures get their own answers.
        auto claimed = std::make_shared<SIPIdentity>("sip:" + credentials->fields["username"] + "@" + realm->name);

        core->account_get(
            claimed, [this, self, request, transaction, realm, caller, credentials, answered, then](plugins::Result<std::shared_ptr<types::Account>> found) {
              if (!found.ok) {
                _logger->error("Could not read an account - " + found.error);
                return _send_status(transaction, request, 500, "Server Internal Error");
              }

              // An unknown user is challenged like a wrong password, so this cannot be used to
              // find out which accounts exist.
              if (!found.value) {
                _logger->info("Request from " + caller->to_string() + " with credentials for no account - challenging");
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

              // Spent. A Digest response is replayable for as long as its nonce lives, and the
              // callee has no use for it.
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

  // Minting writes the nonce to the datastore, and one the store never took would fail its
  // own check when it came back.
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

// RFC 3261 16.6 step 8: "a cryptographic hash of the To tag, From tag, Call-ID header
// field, the Request-URI of the request received (before translation), the topmost Via
// header, and the sequence number from the CSeq header field".
//
// The topmost Via is left out on purpose. It is the hop that handed the request over,
// which is precisely what differs between the first pass and the pass that comes back,
// so including it would make every loop look like a spiral and detect nothing. The
// Route set is in, because 16.3.4 asks for the fields that decide where the request
// goes and that is one of them.
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

  // A strict router moves the top Route into the Request-URI and appends the old
  // Request-URI to the end of the Route set. What arrives is therefore a Request-URI
  // this node once wrote into a Record-Route, which is recognised by naming this node
  // and carrying the lr this node put there. Undoing it is 16.4's first rule.
  if (header->request_uri && header->contains("Route") && header->request_uri->has_parameter("lr") && _names_this_node(*header->request_uri)) {
    const auto& routes = header->headers_map["Route"];
    auto last = routes.back();

    if (auto uri = route_uri(last)) {
      header->request_uri = uri;
      header->remove_value("Route", [&last](std::shared_ptr<headers::Header> value) { return value == last; });
      _logger->debug("Undid a strict router's rewrite - Request-URI is " + header->request_uri->to_string());
    }
  }

  // 16.4's second rule: a Route naming this node has been honoured by arriving here.
  //
  // More than one may, because this node record-routes twice: one value for the
  // interface a request arrived on and one for the interface it left on (RFC 5658).
  // An in-dialog request comes back to whichever of them faces the end that sent it,
  // and the one behind it is this node as well. Both are removed (RFC 5658 section
  // 3.2) or the request would be forwarded to this node.
  //
  // The last one removed is the one facing the other end, and its flow token is how
  // the request gets there. A Contact is not: a browser's resolves to nothing.
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

  // RFC 4028 section 8.1: remembered for the duration of the transaction, and read again
  // in 8.2 when the final response comes back. Taken after _apply_session_timer has had
  // its say, so the interval is the one actually forwarded.
  context->session_timer_supported = has_option_tag(request, "Supported", "timer");
  if (auto* session = session_field_of(request, "Session-Expires"); session != nullptr) context->session_interval = session->delta_seconds;

  // A route set that still has values in it decides the hop, and the Request-URI is
  // left alone (16.6 step 6). This is the whole of in-dialog routing: the Record-Route
  // this node wrote is what put itself in that set.
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

  // The route set is spent and this node was the last hop in it, so what is left is
  // the remote target: the Contact the far end offered. For an endpoint with a
  // routable address that is the answer, and for a browser it is nothing at all -
  // which is what the flow token in the Record-Route this node wrote is for.
  if (auto flow = core->channel_for_token(request->flow_token)) {
    Target target;
    target.uri = request->header->request_uri;
    target.next_hop = request->header->request_uri;
    target.flow = flow;

    context->targets.push_back(target);
    return _forward_next(context);
  }

  // A token whose channel has gone. For UDP the flow has not: it is the pair of addresses,
  // and the far end's NAT still maps it whether or not this node kept a record. The token
  // says which pair it was, so the request goes there and not to a Contact that names the
  // inside of somebody's LAN.
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

  // RFC 3261 16.5: a Request-URI in a domain this element is not responsible for is
  // itself the only target. A realm is what makes a domain one of ours, which is the
  // same question the registrar asks of a REGISTER.
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

    // The one place a realm is already in hand. Reading it again for every message
    // with a body would be a datastore round trip on the media path, and an in-dialog
    // re-INVITE never looks a realm up at all.
    context->media_policy = found.value->behaviour.over(core->config->behaviour);

    // Ours. The Request-URI names an address of record and the bindings the registrar
    // holds for it are the target set.
    auto identity = std::make_shared<SIPIdentity>(request->header->request_uri->to_string());

    core->account_get(identity, [this, self, context](plugins::Result<std::shared_ptr<types::Account>> found) {
      auto core = _core.lock();
      if (!core) return;

      const auto& request = context->request;

      if (!found.ok) {
        _logger->error("Could not read the account for " + request->header->request_uri->to_string() + " - " + found.error);
        return _send_status(context->server, request, 500, "Server Internal Error");
      }

      if (!found.value) {
        _logger->info("No account for " + request->header->request_uri->to_string() + " - 404");
        return _send_status(context->server, request, 404, "Not Found");
      }

      auto account = found.value;
      context->callee_profile = account->media_profile;

      core->location_list(account->id, [this, self, context, account](plugins::Result<std::vector<types::Location>> bindings) {
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

        // RFC 5626: each binding goes back down the flow it was registered over, not down
        // whichever flow the account most recently used. A user registered from a desk
        // phone and a browser has two bindings and two flows, and sending both attempts
        // down one of them reaches one of the two devices twice and the other never.
        for (const auto& binding : bindings.value) {
          if (binding.contact) context->targets.push_back(_target_for(binding));
        }

        _forward_next(context);
      });
    });
  });
}

void Proxy::_forward_next(const std::shared_ptr<Context>& context) {
  auto core = _core.lock();
  if (!core) return;

  // A cancelled fork stops where it is: the caller has already been told 487 and the
  // remaining targets are no longer wanted (16.10).
  if (context->answered || context->cancelled || context->next >= context->targets.size()) return _send_best(context);

  // By value: the continuation below runs after a round trip, and the target set is the
  // context's rather than this frame's.
  const Target target = context->targets[context->next++];
  context->hops_left.clear();
  context->hop_target.reset();
  context->current = target;
  context->offered.reset();

  if (auto channel = target.flow.lock(); channel && channel->_connection) return _forward_to(context, target, channel);

  // RFC 3261 16.6 step 7: a hop this node has no flow to gets one opened. A registered
  // client is answered on the connection it registered over, so this is the trunk, the
  // peer node, and the client whose connection has since closed.
  const auto hop = _next_hop_of(*target.next_hop);

  // An address is where to go already. A name is located first (RFC 3263): NAPTR, SRV,
  // then A and AAAA, giving the places to try in the order to try them (4.3).
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

// Each place a target can be reached, in turn, until one opens.
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

// RFC 3263 4.3, for the branch in flight: the same target at the next place DNS listed,
// when there is one. False leaves the fork to move on as it would have.
bool Proxy::_try_next_hop(const std::shared_ptr<Context>& context) {
  if (context->hops_left.empty() || !context->hop_target || context->cancelled || context->answered) return false;

  auto hops = std::move(context->hops_left);
  context->hops_left.clear();

  _logger->info("Trying " + context->hop_target->next_hop->to_string() + " at the next place DNS listed");
  _connect_hops(context, *context->hop_target, std::move(hops), 0);
  return true;
}

// Unreachable is about this target and not about the request, so the rest of the target
// set still gets its turn (16.7).
void Proxy::_unreachable(const std::shared_ptr<Context>& context) {
  auto unavailable = context->request->generate_response();
  unavailable->header->response_code = 480;
  unavailable->header->response_message = "Temporarily Unavailable";

  if (!context->best) context->best = unavailable;
  _forward_next(context);
}

void Proxy::_forward_to(const std::shared_ptr<Context>& context, const Target& target, const std::shared_ptr<Channel>& channel) {
  // RFC 3261 16.6 step 1: every branch starts from a copy of the request as received,
  // so the Via and the Max-Forwards of one branch are not what the next one inherits.
  auto copy = context->request->clone();

  // The only thing that stops a prepared forward is Max-Forwards, and a request that has
  // run out of hops has run out for every target, so the search ends here (16.3 rule 3).
  if (!_prepare_forward(copy, channel, target, context->loop_token)) {
    _logger->info("Max-Forwards exhausted - 483");
    return _send_status(context->server, context->request, 483, "Too Many Hops");
  }

  // Step 6: the media engine has its say on the body before the copy goes anywhere, and
  // it is a round trip, so the send is the other side of it.
  auto self = shared_from_this();
  _anchor_media(context->request, copy, channel, context, [this, self, context, copy, channel]() { _send_forward(context, copy, channel); });
}

void Proxy::_send_forward(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& copy, const std::shared_ptr<Channel>& channel) {
  auto core = _core.lock();
  if (!core) return;

  // A CANCEL may have arrived while the media engine had the description. The branch is
  // no longer wanted, and 487 has already gone back on the server transaction.
  if (context->answered || context->cancelled) return;

  const bool over_udp = channel->_connection && Util::to_lower(channel->_connection->transport_name()) == "udp";

  // RFC 3261 18.1.1: a request this large may not go out over UDP when the path MTU is
  // unknown. It goes over a congestion controlled transport instead, which for this node
  // means TCP, and the top Via has to say where it really went.
  if (over_udp && wire_size(copy) > kMaxUdpRequest) {
    // The same hop, a different transport. A UDP flow's remote is where the far end's
    // datagrams came from, which for a symmetric endpoint is the port it listens on.
    const auto hop = channel->_connection->remote_endpoint();

    auto self = shared_from_this();

    core->channel_connect("tcp", hop.address().to_string(), hop.port(), [this, self, context, copy, channel](plugins::Result<std::shared_ptr<Channel>> opened) {
      if (opened.ok && opened.value && opened.value->_connection) {
        set_top_via_transport(copy, "TCP");
        return _write_forward(context, copy, opened.value);
      }

      // 18.1.1 again: a TCP attempt the far end refuses is retried over UDP. A
      // datagram that may be fragmented beats a request that never leaves.
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

  // The ACK for a 2xx travels outside any transaction (RFC 3261 17.1.1.3), so it is
  // written straight to the transport.
  if (!context->server) {
    channel->send(copy);
    return;
  }

  context->forwarded = copy;
  context->forwarded_flow = channel;
  context->provisional = false;
  context->timer_c_cancelled = false;

  // Dead entries would otherwise pile up for the life of the node: a context that is
  // answered by a 2xx leaves through _on_response and never comes back here.
  std::erase_if(_contexts, [](const auto& entry) { return entry.second.expired(); });

  _contexts[context->server->id()] = context;

  auto self = shared_from_this();

  context->client = core->client_transaction_start(
      copy, channel, [this, self, context](std::shared_ptr<SIPMessage> response) { _on_response(context, response); },
      [this, self, context]() {
        // Timer B or F. RFC 3261 16.7: a branch that never answered is a 408, and the
        // next target gets its turn.
        _logger->info("No response from target - trying the next");

        auto timeout = context->request->generate_response();
        timeout->header->response_code = 408;
        timeout->header->response_message = "Request Timeout";

        if (!context->best) context->best = timeout;

        _timer_c_cancel(context);
        context->forwarded = nullptr;
        if (_try_next_hop(context)) return;
        _forward_next(context);
      });

  // RFC 3261 16.6 step 11: "Timer C MUST be set for each client transaction when an
  // INVITE request is proxied."
  _timer_c_start(context);
}

void Proxy::_on_response(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& response) {
  if (!context->server) return;

  // RFC 3261 16.7 step 3: the Via this node added on the way out comes off on the way
  // back, so what the caller sees is the chain it sent.
  if (response->header->contains("Via")) {
    auto top = response->header->headers_map["Via"][0];
    response->header->remove_value("Via", [&top](std::shared_ptr<headers::Header> header) { return header == top; });
  }

  const int code = response->header->response_code;

  // RFC 4028 section 8.2 goes first, because the dialog tracker below takes the session
  // interval from what this response says. Running it afterwards would leave this node
  // watching nothing while the caller refreshed on an interval this node had handed it.
  if (is_2xx(code)) _complete_session_timer(context, response);

  // Section 12 is tracked, not routed on: the dialog record is what tells this node a
  // call is up and when it ends, and it is built from what goes past.
  //
  // A branch failing while the fork goes on is that branch's end and not the call's (16.7):
  // the attempt is over when this node sends the caller its final response, and _send_best
  // tells the tracker so then. Once the caller has had its answer - a CANCEL's 487, an
  // earlier branch's 2xx - what a branch says is only the end of that branch.
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

    // 16.7 step 2: a provisional response of 101 to 199 resets timer C, because the
    // branch is demonstrably still working on the call. A 100 Trying is excluded by
    // name - it says the next hop received the INVITE, not that anyone is ringing.
    if (code > 100) _timer_c_start(context);

    // 9.1: a CANCEL waits for a provisional response, because before one there is
    // nothing at the far end that knows the transaction. This is where that wait ends.
    if (context->cancelled) return _cancel_branch(context);

    if (!context->answered) _forward_response(context, response);
    return;
  }

  _timer_c_cancel(context);
  context->forwarded = nullptr;
  context->client = nullptr;

  // The caller already has its final response - a 487 for a CANCEL, or the answer from
  // an earlier branch. What this branch says now is only the end of its own transaction.
  if (context->answered) return;

  // A 2xx ends the search: there is an answer and forking stops (16.7 step 5). A 6xx is
  // a definitive refusal from the user and stops it too.
  if (is_2xx(code) || code >= 600) {
    if (is_2xx(code)) _report_reoffer(context, true);
    context->best = response;
    context->answered = true;
    _contexts.erase(context->server->id());
    _forward_response(context, response);
    return;
  }

  // Otherwise remember it and try the next binding. Serial forking: lowest code wins,
  // which for the codes that reach here is the closest to an answer.
  if (!context->best || code < context->best->header->response_code) context->best = response;

  // RFC 3263 4.3: a 503 is that server failing, and the next server for the same target may
  // not be. Anything else is an answer, which another server would give as well.
  if (code == 503 && _try_next_hop(context)) return;

  if (code == 488) {
    if (_reoffer(context)) return;
    _report_reoffer(context, false);
  }

  _forward_next(context);
}

// Kamailio's failure-route pattern, and step 4 of the 2026-10-01 decision. A 488 (RFC 3261
// 21.4.26) to an offer this node's engine produced for a leg that had not described itself
// is a refusal of this node's guess rather than of the caller, so the same target is offered
// the other profile, once. RFC 3264 is untouched: the callee sees two offers on two
// transactions and refused one of them.
bool Proxy::_reoffer(const std::shared_ptr<Context>& context) {
  if (context->current.profile || !context->offered) return false;

  std::optional<media::Profile> other;
  if (*context->offered == media::Profile::PlainRtp) other = media::Profile::WebRtc;
  if (*context->offered == media::Profile::WebRtc) other = media::Profile::PlainRtp;
  if (!other) return false;

  Target again = context->current;
  again.profile = other;
  again.rejected = context->offered;
  context->targets.insert(context->targets.begin() + static_cast<std::ptrdiff_t>(context->next), again);

  _logger->info(context->request->header->request_uri->to_string() + " refused the offer it was made - offering the other profile");
  _forward_next(context);
  return true;
}

// What the re-offer came to, for the operator. Nothing is learned from it: the node says
// which account needed the other profile and the operator decides whether to set it.
void Proxy::_report_reoffer(const std::shared_ptr<Context>& context, bool took) {
  const auto& current = context->current;
  if (!current.profile || !current.rejected) return;

  auto core = _core.lock();
  if (!core) return;

  const auto account = context->request->header->request_uri->to_string();
  core->reoffers().record(account, *current.rejected, took ? current.profile : std::nullopt);

  if (took) _logger->warn(account + " took the other media profile after refusing the first - see /api/v1/media/reoffers");
}

void Proxy::_send_best(const std::shared_ptr<Context>& context) {
  if (!context->server || context->answered) return;

  _contexts.erase(context->server->id());

  if (!context->best) return _send_status(context->server, context->request, 480, "Temporarily Unavailable");

  // RFC 3261 16.7 step 6: a 503 says the next hop is out of service, which is about the
  // hop and not about the request. Passing it upstream would tell the caller something
  // untrue about this node, so it goes back as a 500.
  if (context->best->header->response_code == 503) {
    context->answered = true;
    return _send_status(context->server, context->request, 500, "Server Internal Error");
  }

  // Observing again is harmless: a dialog already terminated is gone from the table, and
  // one that was never created has nothing to end. What it catches is the response this
  // node made up rather than received, such as the 408 timer B produced.
  if (auto core = _core.lock()) core->dialogs()->observe_response(context->request, context->best);

  context->answered = true;
  _forward_response(context, context->best);
}

void Proxy::_forward_response(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& response) {
  auto self = shared_from_this();
  auto server = context->server;

  // A response goes back down the flow the request came in on, so that flow is the one
  // whose transport says what the far leg of this direction is.
  _anchor_media(context->request, response, context->request->channel.lock(), context, [self, server, response]() { server->send(response); });
}

void Proxy::_anchor_media(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<SIPMessage>& message, const std::shared_ptr<Channel>& outgoing,
                          const std::shared_ptr<Context>& context, std::function<void()> then) {
  auto core = _core.lock();
  if (!core || !core->media || !has_sdp(message)) return then();

  auto call = core->call_get(value_of(request, "Call-ID"));
  auto dialog = core->dialogs()->find(request);

  // No call record and no dialog means nothing to anchor against - which end sent this
  // description is the one question the engine has to be told the answer to.
  if (!call || !dialog) return then();

  // The policy is the realm's, decided once where the realm was in hand and kept on
  // the call, because a re-INVITE arrives in-dialog with no realm to ask.
  if (context && context->media_policy) call->media_policy = *context->media_policy;

  // Likewise the callee's account, kept on its leg.
  if (context && context->callee_profile) {
    if (const auto callee = call->participant_index(false)) call->participants[*callee].account_profile = context->callee_profile;
  }

  const auto& policy = call->media_policy;

  // A realm that does not want its media anchored gets what a node with no engine
  // gives it: the description travels exactly as it arrived and the media goes end to
  // end. That is the proxy behaviour RFC 3261 16.6 describes, chosen rather than
  // fallen into.
  if (!policy.anchor) return then();

  const bool is_response = message->header->type == SIPHeader::Type::Response;
  const bool request_from_caller = dialog->is_from_caller(tag_of(request, "From"));

  // A response carries the answering end's description, which is the end the request did
  // not come from.
  const bool from_caller = is_response ? !request_from_caller : request_from_caller;

  const auto participant = call->participant_index(from_caller);
  if (!participant) return then();

  // And the leg this description is being produced for is the other one, which is the
  // leg whose profile says what the engine has to make.
  const auto recipient = call->participant_index(!from_caller);

  // RFC 3264 section 5: the INVITE carries the offer and the response to it carries the
  // answer. An INVITE with no description at all inverts that - the response becomes the
  // offer and the ACK the answer - which is why the ACK is not an offer here.
  const bool is_offer = is_response ? !has_sdp(request) : message->header->request_method != "ACK";

  auto flags = media::Flags::from_sdp(message->body);
  flags.participant = *participant;

  // What this leg is, from its own mouth, and it is read before the engine rewrites the
  // body: an offer or an answer says exactly whether the end that wrote it asked for
  // ICE, DTLS or SRTP. It outlives the message, because the next description this node
  // has to produce for that leg is then produced from what it said rather than guessed
  // at a second time.
  if (const auto stated = flags.stated()) call->participants[*participant].profile = *stated;

  // What the far leg needs, which the description in hand cannot say. What that leg has
  // already told this node comes first: a delayed offer, an answer going back to the end
  // that offered and every re-INVITE after are all a leg this node has heard describe
  // itself. Then the leg's account, where the operator has said what the endpoint is.
  // The realm's setting, and where it has none the transport of the outgoing flow,
  // decide only for the first description produced towards a leg nobody has spoken for.
  const bool unheard = !(recipient && call->participants[*recipient].profile);

  if (!is_response && context && context->current.profile) {
    // The re-offer a 488 earned, which is the one thing that outranks the rest.
    flags.target = *context->current.profile;
  } else if (!unheard) {
    flags.target = *call->participants[*recipient].profile;
  } else if (recipient && call->participants[*recipient].account_profile) {
    types::MediaPolicy account;
    account.profiles = *call->participants[*recipient].account_profile;
    flags.target = _profile_under(account, outgoing && outgoing->_connection ? outgoing->_connection->transport_name() : std::string());
  } else {
    flags.target = _profile_under(policy, outgoing && outgoing->_connection ? outgoing->_connection->transport_name() : std::string());
  }

  // What the offer towards an unheard callee is being made as, so that a refusal of it can
  // be answered with the other. Mirror is whatever the caller said.
  std::optional<media::Profile> offered;
  if (!is_response && is_offer && unheard && context && message->header->request_method == "INVITE") {
    offered = flags.target == media::Profile::Mirror ? flags.stated() : std::optional<media::Profile>(flags.target);
  }

  auto self = shared_from_this();

  auto handler = [this, self, message, context, offered, then = std::move(then)](media::Result result) {
    if (result.ok) {
      // Only an offer the engine made counts as this node's; one passed through untouched
      // is the caller's, and a refusal of it is the caller's to hear.
      if (context && offered) context->offered = offered;
      message->body = std::move(result.sdp);
      message->body_length = static_cast<unsigned int>(message->body.size());
    } else {
      // The engine will not take it - a WebRTC offer at the plain-RTP relay, or no ports
      // left. Passing the description through untouched is what a proxy would have done
      // with it in any case, and it beats failing a call this node can still signal.
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

  // RFC 3261 16.6 step 2: the Request-URI becomes the target this hop is for.
  if (target.uri) header->request_uri = target.uri;

  // Step 3: Max-Forwards is decremented, and a request that has run out cannot be
  // forwarded (16.3 rule 3 answers it 483).
  if (header->contains("Max-Forwards")) {
    auto max_forwards = header->headers_map["Max-Forwards"][0]->as<UIntHeader>();
    if (max_forwards != nullptr) {
      if (max_forwards->value == 0) return false;
      max_forwards->value--;
    }
  } else {
    header->add("Max-Forwards", std::make_shared<UIntHeader>(kDefaultMaxForwards));
  }

  const auto local = channel->_connection->local_endpoint();
  const auto transport = Util::to_lower(channel->_connection->transport_name());

  // Step 4: Record-Route, on the requests that can start a dialog. It is what brings the
  // ACK, the BYE and every re-INVITE back through this node, which a node that anchors
  // media and keeps call records has to have.
  //
  // Two values, and always two. RFC 5658 requires it of a proxy whose request arrives
  // on one interface and leaves on another, which a browser calling a desk phone always
  // is. Writing two whatever the transports are costs one header and buys the thing
  // that actually makes such a call work: each value carries the flow token of the side
  // it faces, so an in-dialog request can be sent back down a flow rather than to a
  // Contact. A browser's Contact resolves to nothing, so without this the ACK and the
  // BYE never reach it, whatever the transports were.
  //
  // The topmost names the interface the request is being sent on and the second the one
  // it arrived on (RFC 5658 section 3), which is the order that gives each end the
  // value facing it: a UAS takes the route set from the request in order, and a UAC
  // takes it from the response reversed (RFC 3261 12.1.1, 12.1.2).
  if (header->request_method == "INVITE") {
    const bool secure_request = Util::to_lower(header->request_uri->scheme) == "sips";
    auto inbound = copy->channel.lock();

    auto record_route_for = [&](const std::string& interface_transport, const std::string& address, std::uint16_t port, const std::string& token) {
      auto uri = std::make_shared<SIPUri>();
      uri->valid = true;
      uri->scheme = (interface_transport == "tls" || secure_request) ? "sips" : "sip";
      uri->host = core->advertised_address(address);
      uri->port = port;

      // RFC 5626 section 5.1: the flow token goes in the user part, where it is opaque
      // to everyone but the node that wrote it.
      uri->user = token;

      // 19.1.1: lr says this node is a loose router, which is what stops the next hop
      // rewriting the Request-URI on its way back.
      uri->set_parameter("lr", "");
      if (interface_transport != "udp") uri->set_parameter("transport", interface_transport);

      auto identity = std::make_shared<SIPIdentity>();
      identity->wrapped = true;
      identity->uri = uri;

      return std::make_shared<SIPIdentityHeader>(identity);
    };

    // Added bottom first, because add_start puts each one in front of the last.
    if (inbound && inbound->_connection) {
      const auto arrived_on = inbound->_connection->local_endpoint();
      header->add_start("Record-Route", record_route_for(Util::to_lower(inbound->_connection->transport_name()), arrived_on.address().to_string(),
                                                         arrived_on.port(), inbound->flow_token()));
    }

    header->add_start("Record-Route", record_route_for(transport, local.address().to_string(), local.port(), channel->flow_token()));
  }

  // Step 6: a top Route without lr belongs to a strict router, which expects to find
  // its own URI in the Request-URI. The one this node is forwarding to goes to the end
  // of the route set so that it is not lost on the way.
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

  // Step 8: this node's Via goes on top. The branch is the magic cookie (8.1.1.7), the
  // loop token and a value of its own, so that two branches of one fork are different
  // transactions while both still say where they have been.
  const auto branch = std::string(kMagicCookie) + loop_token + "." + Util::generate_random_string("", 12);

  auto via = std::make_shared<ViaHeader>("SIP/2.0/" + Util::to_upper(transport) + " " + core->advertised_address(local.address().to_string()) + ":" +
                                         std::to_string(local.port()) + ";branch=" + branch);

  header->add_start("Via", via);
  copy->branch = branch;

  return true;
}

void Proxy::on_cancel(std::shared_ptr<SIPMessage> cancel, std::shared_ptr<transactions::TransactionBase> cancel_transaction,
                      std::shared_ptr<transactions::TransactionBase> invite_transaction) {
  // RFC 3261 9.2: a CANCEL this node has a response context for is answered here,
  // because this node is the one that will act on it. One it has no context for is
  // forwarded instead, and answering it as well would send the caller two answers to
  // one request.
  if (cancel_transaction && invite_transaction) {
    auto ok = cancel->generate_response();
    ok->header->response_code = 200;
    ok->header->response_message = "OK";
    cancel_transaction->send(ok);
  }

  if (!invite_transaction) return _forward_cancel_statelessly(cancel, cancel_transaction);

  // RFC 3261 16.10: the branches this node has already tried are cancelled in turn.
  // Without this the callee keeps ringing after the caller has hung up.
  auto search = _contexts.find(invite_transaction->id());

  if (search != _contexts.end()) {
    if (auto context = search->second.lock()) {
      context->cancelled = true;
      context->answered = true;
      _cancel_branch(context);
    }

    _contexts.erase(search);
  }

  // The INVITE it cancels ends with 487, which is what the caller is waiting for.
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

  // A request that offered no interval gets this node's, which is 8.1's other half.
  if (session == nullptr || session->delta_seconds == 0) {
    _insert_session_timer(request);
    return true;
  }

  const std::uint32_t minimum = core->config->sip_session_min_se;
  if (minimum == 0 || session->delta_seconds >= minimum) return true;

  // 8.1: "If the request contains a Supported header field with a value 'timer', the
  // proxy MAY reject the INVITE request with a 422 (Session Interval Too Small) response
  // if the session interval in the Session-Expires header field is smaller than the
  // minimum interval defined by the proxy's local policy." A caller that understands
  // session timers understands the 422, and will come back with an interval this node
  // will hold state for.
  if (has_option_tag(request, "Supported", "timer")) {
    _logger->info("Session-Expires of " + std::to_string(session->delta_seconds) + "s is below this node's minimum - 422");
    _send_interval_too_small(transaction, request, minimum);
    return false;
  }

  // 8.1 again, for a caller that does not: "the proxy cannot usefully reject the request,
  // as this would result in a call failure. Rather, the proxy SHOULD insert a Min-SE
  // header field containing its minimum interval." The 422 would be answered by nobody,
  // so the interval is raised on the way through instead and the far end is told what the
  // floor was. A Min-SE already in the request is raised and never lowered.
  std::uint32_t floor = minimum;

  if (auto* min_se = session_field_of(request, "Min-SE"); min_se != nullptr) {
    if (min_se->delta_seconds < minimum) min_se->delta_seconds = minimum;
    floor = min_se->delta_seconds;
  } else {
    request->header->add("Min-SE", std::make_shared<headers::SessionExpiresHeader>(minimum));
  }

  // "The proxy MUST then increase the Session-Expires header field value to be equal to
  // the value in the Min-SE header field." The refresher parameter is not touched: 8.1
  // says in as many words that it is not the proxy's to insert or modify.
  _logger->info("Raising a Session-Expires of " + std::to_string(session->delta_seconds) + "s to " + std::to_string(floor) +
                "s - the caller does not support timer");

  session->delta_seconds = floor;
  return true;
}

void Proxy::_insert_session_timer(const std::shared_ptr<SIPMessage>& request) {
  auto core = _core.lock();
  if (!core) return;

  // Nothing to insert, or something already there to leave alone. A Session-Expires of
  // zero is malformed rather than absent, and adding a second header beside it would make
  // it worse.
  if (core->config->sip_session_expires == 0) return;
  if (session_field_of(request, "Session-Expires") != nullptr) return;

  // 8.1: the interval "may contain any desired expiration time the proxy would like, but
  // not with a duration lower than the value in the Min-SE header field in the request,
  // if it is present".
  std::uint32_t interval = core->config->sip_session_expires;
  if (auto* min_se = session_field_of(request, "Min-SE"); min_se != nullptr && min_se->delta_seconds > interval) interval = min_se->delta_seconds;

  // "The proxy MUST NOT include a refresher parameter in the header field value." Which
  // end refreshes is settled between the endpoints in the 2xx (section 9 Table 2), and a
  // UAS whose caller does not understand session timers takes it on itself - which is
  // what makes inserting one safe rather than a way of breaking calls to old equipment.
  request->header->add("Session-Expires", std::make_shared<headers::SessionExpiresHeader>(interval));

  _logger->debug("Call asked for no session interval - offering " + std::to_string(interval) + "s");

  // 8.1: "If the request did not contain a Supported header field with the value 'timer',
  // the proxy MAY insert a Require header field with the value 'timer' into the request.
  // However, this is NOT RECOMMENDED. This allows the proxy to insist on a session timer
  // for the session." Insisting means a callee that does not implement RFC 4028 answers
  // 420 Bad Extension and the call fails, rather than going on without an expiry, which
  // is why it is off unless somebody has said they want it.
  if (!core->config->sip_require_session_timer) return;
  if (has_option_tag(request, "Supported", "timer")) return;
  if (has_option_tag(request, "Require", "timer")) return;

  _logger->debug("Requiring the session timer this node offered");
  request->header->add("Require", "timer");
}

void Proxy::_complete_session_timer(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& response) {
  if (!is_session_refresh(context->request->header->request_method)) return;

  // RFC 4028 section 8.2: "If the received response contains a Session-Expires header
  // field, no modification of the response is needed."
  if (session_field_of(response, "Session-Expires") != nullptr) return;

  // The callee said nothing about session timers. If the caller said nothing either then
  // there is no session expiration and the response goes up as it arrived; this node does
  // not invent one for two ends that never asked.
  if (!context->session_timer_supported || context->session_interval == 0) return;

  // "Because there is no Session-Expires or Require header field in the response, the
  // proxy knows that it is the first session-timer-aware proxy to receive the response.
  // This proxy MUST insert a Session-Expires header field into the response with the
  // value it remembered from the forwarded request."
  auto session = std::make_shared<headers::SessionExpiresHeader>(context->session_interval);

  // "It MUST set the value of the 'refresher' parameter to 'uac'." The callee cannot
  // refresh a session it does not know it has.
  session->refresher = "uac";
  response->header->add("Session-Expires", session);

  // "The proxy MUST add the 'timer' option tag to any Require header field in the
  // response, and if none was present, add the Require header field with that value."
  if (!has_option_tag(response, "Require", "timer")) response->header->add("Require", "timer");

  _logger->info("Callee answered without a session timer - telling the caller its own interval stands, refreshed by it");
}

void Proxy::_send_interval_too_small(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                                     std::uint32_t minimum) {
  if (!transaction) return;

  auto response = request->generate_response();
  response->header->response_code = 422;
  response->header->response_message = "Session Interval Too Small";

  // RFC 4028 section 6: "The 422 response MUST contain a Min-SE header field with the
  // minimum timer for that server." Without it the caller has nothing to retry with.
  response->header->add("Min-SE", std::make_shared<headers::SessionExpiresHeader>(minimum));

  if (auto core = _core.lock()) core->dialogs()->observe_response(request, response);

  transaction->send(response);
}

void Proxy::_timer_c_start(const std::shared_ptr<Context>& context) {
  _timer_c_cancel(context);

  auto core = _core.lock();

  // 16.6 step 11 sets the timer for a client transaction, so that is what it is tied to
  // and not to the copy of the request: _cancel_branch lets go of the copy once the
  // CANCEL is away, and the branch it was cancelling is still outstanding after that.
  if (!core || !context->client) return;

  // Only an INVITE gets one. It is the only method whose client transaction can sit in
  // Proceeding indefinitely; a non-INVITE branch is bounded by timer F whatever it
  // answers (RFC 3261 17.1.2.2).
  if (context->request->header->request_method != "INVITE") return;

  auto timers = core->timer_source();
  if (!timers) return;

  // Weak on both sides: a timer set for four minutes must not be what keeps the node's
  // proxy or a finished response context alive.
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

  // RFC 3261 16.8, the first half: "If the client transaction has received a provisional
  // response, the proxy MUST generate a CANCEL request matching that transaction." The
  // far end believes it is ringing somebody, and dropping the branch silently would
  // leave it ringing. 16.8 offers a reset of the timer instead of terminating the
  // transaction, and that is what happens here, so a far end that answers the CANCEL
  // ends its branch the ordinary way, through a 487 and _on_response.
  if (context->provisional && !context->timer_c_cancelled) {
    _logger->info("Timer C fired on a branch that is still provisional - cancelling it");

    context->timer_c_cancelled = true;
    _cancel_branch(context);
    _timer_c_start(context);
    return;
  }

  // The second half, and the other end of that choice. Either the branch never answered
  // at all, in which case 16.8 says to behave as though a 408 had come back, or it
  // ignored the CANCEL and its transaction is terminated here rather than left waiting
  // on a response that is not coming.
  _logger->info("Timer C fired on a branch that will not finish - giving up on it");

  if (context->client) {
    context->client->terminate();
    context->client = nullptr;
  }

  context->forwarded = nullptr;

  // The caller has its answer already; this is only the end of a branch it is no longer
  // waiting on.
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

  // RFC 3261 9.1: a CANCEL for a branch that has not answered yet would name a
  // transaction the far end does not have. It waits for the provisional response.
  if (!context->provisional || !context->forwarded) return;

  auto channel = context->forwarded_flow.lock();
  if (!channel) return;

  const auto& forwarded = context->forwarded;

  // 9.1: the CANCEL is the request being cancelled with its method changed, a single
  // Via - the one this node put on - and the Route set it was sent with. Everything
  // that describes the body goes, because a CANCEL has none.
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

  // The CANCEL is a transaction of its own (9.1), sharing the branch of the INVITE it
  // names. The method is part of a transaction's identity, so the two do not collide.
  core->client_transaction_start(cancel, channel, nullptr, nullptr);

  context->forwarded = nullptr;
}

// RFC 3261 16.10: "If a response context is not found, the element does not have any
// knowledge of the request to apply the CANCEL to. It MUST statelessly forward the
// CANCEL request." The reason is that the request it names may have been forwarded
// statelessly, in which case there is nothing here to have kept.
//
// Answering it 200 and dropping it, which is what this did, tells the caller the
// branch was cancelled when nothing downstream has been told anything.
void Proxy::_forward_cancel_statelessly(const std::shared_ptr<SIPMessage>& cancel, const std::shared_ptr<transactions::TransactionBase>& transaction) {
  auto core = _core.lock();
  if (!core) return;

  // The same 16.4 processing any request gets: this node's own Route values have been
  // honoured by the request arriving, and the last of them names the flow to use.
  _preprocess_routes(cancel);

  std::shared_ptr<SIPUri> next_hop;
  if (cancel->header->contains("Route")) next_hop = route_uri(cancel->header->headers_map["Route"][0]);
  if (!next_hop) next_hop = cancel->header->request_uri;

  auto flow = next_hop ? _flow_to(*next_hop) : nullptr;
  if (!flow) flow = core->channel_for_token(cancel->flow_token);

  if (!flow) {
    // Nowhere to send it. RFC 3261 9.2's answer for a CANCEL that matches nothing is
    // 481, which is true of this node in a way that 200 was not.
    _logger->info("CANCEL with no response context and nowhere to forward it - 481");
    return _send_status(transaction, cancel, 481, "Call/Transaction Does Not Exist");
  }

  // 16.11: a stateless forward computes its branch from the request rather than at
  // random, so a retransmitted CANCEL is forwarded as the same transaction rather than
  // as a new one the far end has never seen.
  const auto branch = std::string(kMagicCookie) + _loop_token(cancel);

  const auto local = flow->_connection->local_endpoint();
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

  auto via = std::make_shared<ViaHeader>("SIP/2.0/" + Util::to_upper(transport) + " " + core->advertised_address(local.address().to_string()) + ":" +
                                         std::to_string(local.port()) + ";branch=" + branch);

  cancel->header->add_start("Via", via);
  cancel->branch = branch;

  _logger->info("CANCEL with no response context, forwarded statelessly to " + flow->flow_id());

  // No client transaction and no response context: the answer comes back matching
  // neither and goes upstream through the stateless response path (16.7 step 1),
  // which strips the Via added just above.
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

  // RFC 3261 18.1.2: a response whose top Via is not one this node wrote was never
  // ours to forward, whatever it claims to answer.
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

  // 18.2.2: the response goes where the request came from, which is received and rport
  // when the hop below asked to be told, and its sent-by otherwise.
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

  // An address and a port, not a record this node has to be keeping: a UDP flow forgotten
  // by the idle sweep is reopened from the listener's socket, and for a connection that has
  // gone 18.2.2 says to open one. Whatever cannot be reached is dropped, as a stateless
  // proxy would.
  const auto name = host + ":" + std::to_string(port);

  core->channel_connect(transport, host, port, [this, self = shared_from_this(), response, name](plugins::Result<std::shared_ptr<Channel>> opened) {
    if (!opened.ok || !opened.value) {
      _logger->info("No flow back to " + name + " for a stray response - dropping - " + opened.error);
      return;
    }

    opened.value->send(response);
  });
}

bool Proxy::_names_this_node(const SIPUri& uri) const {
  auto core = _core.lock();
  if (!core) return false;

  const auto hop = _next_hop_of(uri);
  return core->is_local_address(hop.host, hop.port);
}

// What the realm says, and only where it says nothing specific does the transport
// decide. A WebSocket means a browser almost everywhere, and "almost" is why a realm
// can say otherwise: RFC 7118 is SIP over WebSocket and requires no WebRTC at all.
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
  // RFC 5626 section 5.3: the Request-URI is the Contact the binding registered, whichever
  // way the request is then sent.
  Target target;
  target.uri = binding.contact;
  target.next_hop = binding.contact;

  auto core = _core.lock();
  if (!core) return target;

  // The flow the registration was made over, when it is still open. This is the whole of
  // RFC 5626's routing: a browser's Contact URI has nothing listening behind it and a
  // NAT'd client's names the wrong side of the NAT, so the connection they registered on
  // is the only way back to either.
  if (auto flow = core->channel_find(binding.flow_id)) {
    target.flow = flow;
    return target;
  }

  // A UDP flow this node has forgotten is one it can still send down (section 3.1: the flow
  // is the pair of addresses). Only from the node that held it, though: from anywhere else
  // the datagram comes from an address the far end's NAT has never seen.
  const bool held_here = binding.node_id.empty() || binding.node_id == core->config->sip_node_id;

  if (auto hop = held_here ? _datagram_hop(binding.flow_id) : nullptr) {
    target.next_hop = hop;
    target.flow = _flow_to(*hop);
    return target;
  }

  // No flow recorded, or a reliable one that has since closed. The Contact is all there is,
  // which is right for a desk phone with a routable address and hopeless for a browser - and
  // a binding whose connection has gone is a binding whose client has gone, so the attempt
  // fails and the fork moves on to the next one. Answering 430 Flow Failed instead is RFC
  // 5626 section 11 and wants the registrar to act on it.
  target.flow = _flow_to(*binding.contact);
  return target;
}

// The next hop a UDP flow id names, for sending down a flow this node no longer holds a
// channel for. Null for anything that is not a UDP flow: a TCP, TLS or WebSocket flow that
// has closed is gone, and only its client can open another.
std::shared_ptr<SIPUri> Proxy::_datagram_hop(const std::string& flow_id) {
  static const std::string kScheme = Core::channel_key("udp", "");

  if (flow_id.rfind(kScheme, 0) != 0) return nullptr;

  // host:port, where the host may be an IPv6 address and so the port is after the last
  // colon rather than the first.
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

// RFC 3261 16.6 step 7, without RFC 3263: the URI says the transport and the port, and
// what it does not say has a default. NAPTR and SRV resolution is its own step.
Proxy::NextHop Proxy::_next_hop_of(const SIPUri& uri) {
  NextHop hop;

  const bool secure = Util::to_lower(uri.scheme) == "sips";

  hop.transport = Util::to_lower(uri.parameter("transport"));
  if (hop.transport.empty()) hop.transport = secure ? "tls" : "udp";

  // There is one WebSocket flow underneath ws and wss; which of the two it is, is a
  // property of the listener and not of the connection a message travels on.
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

  // A failure this node answered itself ends the attempt as surely as one from a branch,
  // and nothing else would tell the tracker it is over.
  if (auto core = _core.lock()) core->dialogs()->observe_response(request, response);

  transaction->send(response);
}

}  // namespace athenasip
