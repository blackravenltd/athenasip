//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "proxy.h"

#include <utility>

#include "channel.h"
#include "core.h"
#include "headers/cseq_header.h"
#include "headers/sip_identity_header.h"
#include "headers/uint_header.h"
#include "headers/via_header.h"
#include "loggers/logger_scoped.h"
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

  // RFC 3261 16.4, then 16.5.
  _preprocess_routes(request);

  _determine_targets(request, transaction, token);
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
  if (!header->contains("Route")) return;

  auto top = header->headers_map["Route"][0];
  auto uri = route_uri(top);

  if (uri && _names_this_node(*uri)) {
    header->remove_value("Route", [&top](std::shared_ptr<headers::Header> value) { return value == top; });
  }
}

void Proxy::_determine_targets(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction,
                               const std::string& loop_token) {
  auto core = _core.lock();
  if (!core) return;

  auto context = std::make_shared<Context>();
  context->request = request;
  context->server = transaction;
  context->loop_token = loop_token;

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

    // Ours. The Request-URI names an address of record and the bindings the registrar
    // holds for it are the target set.
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

        // One node, one flow per subscriber: the request goes back down the connection
        // the callee registered on. Per-binding flow routing is RFC 5626, and
        // Location::flow_id exists for it.
        auto flow = core->subscriber_get_channel(subscriber);

        for (const auto& binding : bindings.value) {
          Target target;
          target.uri = binding.contact;
          target.next_hop = binding.contact;
          target.flow = flow ? flow : _flow_to(*binding.contact);

          context->targets.push_back(target);
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

  const auto& target = context->targets[context->next++];

  auto channel = target.flow.lock();

  if (!channel || !channel->_connection) {
    // The next hop is not one this node has a live flow to - either it never had one, or
    // the connection has since closed - and opening one needs outbound connections, which
    // the transports step adds. Try the rest of the target set rather than ending the
    // search on it.
    _logger->info("No flow to " + target.next_hop->to_string() + " - trying the next target");

    auto unavailable = context->request->generate_response();
    unavailable->header->response_code = 480;
    unavailable->header->response_message = "Temporarily Unavailable";

    if (!context->best) context->best = unavailable;
    return _forward_next(context);
  }

  // RFC 3261 16.6 step 1: every branch starts from a copy of the request as received,
  // so the Via and the Max-Forwards of one branch are not what the next one inherits.
  auto copy = context->request->clone();

  // The only thing that stops a prepared forward is Max-Forwards, and a request that has
  // run out of hops has run out for every target, so the search ends here (16.3 rule 3).
  if (!_prepare_forward(copy, channel, target, context->loop_token)) {
    _logger->info("Max-Forwards exhausted - 483");
    return _send_status(context->server, context->request, 483, "Too Many Hops");
  }

  // The ACK for a 2xx travels outside any transaction (RFC 3261 17.1.1.3), so it is
  // written straight to the transport.
  if (!context->server) {
    channel->send(copy);
    return;
  }

  context->forwarded = copy;
  context->forwarded_flow = channel;
  context->provisional = false;

  // Dead entries would otherwise pile up for the life of the node: a context that is
  // answered by a 2xx leaves through _on_response and never comes back here.
  std::erase_if(_contexts, [](const auto& entry) { return entry.second.expired(); });

  _contexts[context->server->id()] = context;

  auto self = shared_from_this();

  core->client_transaction_start(
      copy, channel, [this, self, context](std::shared_ptr<SIPMessage> response) { _on_response(context, response); },
      [this, self, context]() {
        // Timer B or F. RFC 3261 16.7: a branch that never answered is a 408, and the
        // next target gets its turn.
        _logger->info("No response from target - trying the next");

        auto timeout = context->request->generate_response();
        timeout->header->response_code = 408;
        timeout->header->response_message = "Request Timeout";

        if (!context->best) context->best = timeout;

        context->forwarded = nullptr;
        _forward_next(context);
      });
}

void Proxy::_on_response(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& response) {
  if (!context->server) return;

  // RFC 3261 16.7 step 3: the Via this node added on the way out comes off on the way
  // back, so what the caller sees is the chain it sent.
  if (response->header->contains("Via")) {
    auto top = response->header->headers_map["Via"][0];
    response->header->remove_value("Via", [&top](std::shared_ptr<headers::Header> header) { return header == top; });
  }

  // Section 12 is tracked, not routed on: the dialog record is what tells this node a
  // call is up and when it ends, and it is built from what goes past.
  if (auto core = _core.lock()) core->dialogs()->observe_response(context->request, response);

  const int code = response->header->response_code;

  if (!is_final(code)) {
    context->provisional = true;

    // 9.1: a CANCEL waits for a provisional response, because before one there is
    // nothing at the far end that knows the transaction. This is where that wait ends.
    if (context->cancelled) return _cancel_branch(context);

    if (!context->answered) context->server->send(response);
    return;
  }

  context->forwarded = nullptr;

  // The caller already has its final response - a 487 for a CANCEL, or the answer from
  // an earlier branch. What this branch says now is only the end of its own transaction.
  if (context->answered) return;

  // A 2xx ends the search: there is an answer and forking stops (16.7 step 5). A 6xx is
  // a definitive refusal from the user and stops it too.
  if (is_2xx(code) || code >= 600) {
    context->best = response;
    context->answered = true;
    _contexts.erase(context->server->id());
    context->server->send(response);
    return;
  }

  // Otherwise remember it and try the next binding. Serial forking: lowest code wins,
  // which for the codes that reach here is the closest to an answer.
  if (!context->best || code < context->best->header->response_code) context->best = response;

  _forward_next(context);
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
  context->server->send(context->best);
}

bool Proxy::_prepare_forward(const std::shared_ptr<SIPMessage>& copy, const std::shared_ptr<Channel>& channel, const Target& target,
                             const std::string& loop_token) const {
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
  // One value, naming the flow this hop goes out on. A call whose two ends are on
  // different transports needs two (RFC 5658), and that waits for the transports step.
  if (header->request_method == "INVITE") {
    auto record_route = std::make_shared<SIPUri>();
    record_route->valid = true;
    record_route->scheme = (transport == "tls" || Util::to_lower(header->request_uri->scheme) == "sips") ? "sips" : "sip";
    record_route->host = local.address().to_string();
    record_route->port = local.port();

    // 19.1.1: lr says this node is a loose router, which is what stops the next hop
    // rewriting the Request-URI on its way back.
    record_route->set_parameter("lr", "");
    if (transport != "udp") record_route->set_parameter("transport", transport);

    auto identity = std::make_shared<SIPIdentity>();
    identity->wrapped = true;
    identity->uri = record_route;

    header->add_start("Record-Route", std::make_shared<SIPIdentityHeader>(identity));
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

  auto via = std::make_shared<ViaHeader>("SIP/2.0/" + Util::to_upper(transport) + " " + local.address().to_string() + ":" + std::to_string(local.port()) +
                                         ";branch=" + branch);

  header->add_start("Via", via);
  copy->branch = branch;

  return true;
}

void Proxy::on_cancel(std::shared_ptr<SIPMessage> cancel, std::shared_ptr<transactions::TransactionBase> cancel_transaction,
                      std::shared_ptr<transactions::TransactionBase> invite_transaction) {
  // RFC 3261 9.2: the CANCEL is answered on its own transaction whether or not it names
  // anything we still hold.
  if (cancel_transaction) {
    auto ok = cancel->generate_response();
    ok->header->response_code = 200;
    ok->header->response_message = "OK";
    cancel_transaction->send(ok);
  }

  if (!invite_transaction) {
    _logger->info("CANCEL for an unknown transaction - answered 200 and dropped");
    return;
  }

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
  if (rport != next_via->parameters.end() && !rport->second.empty()) port = static_cast<std::uint16_t>(std::stoul(rport->second));

  auto channel = core->channel_find(transport, host, port);

  if (!channel) {
    _logger->info("No flow back to " + host + ":" + std::to_string(port) + " for a stray response - dropping");
    return;
  }

  channel->send(response);
}

bool Proxy::_names_this_node(const SIPUri& uri) const {
  auto core = _core.lock();
  if (!core) return false;

  const auto hop = _next_hop_of(uri);
  return core->is_local_address(hop.host, hop.port);
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
