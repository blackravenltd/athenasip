//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "cli_explain.h"

#include <algorithm>
#include <future>
#include <sstream>

#include "channel.h"
#include "core.h"
#include "headers/sip_identity_header.h"
#include "policy/policy.h"
#include "servers/connection.h"
#include "sip_message.h"
#include "types/realm.h"
#include "types/subscriber.h"
#include "util.h"

namespace athenasip::cli {

namespace {

// A connection with an address and nothing behind it: the request's source, for a policy that asks.
class StandIn final : public servers::Connection {
 public:
  StandIn(ExplainSource source) : _source(std::move(source)), _endpoint(boost::asio::ip::make_address(_source.address), _source.port) {}

  bool start() override { return true; }
  boost::asio::any_io_executor executor() override { return detail::get_global_io_context().get_executor(); }
  void async_read_some(boost::asio::mutable_buffer, std::function<void(const boost::system::error_code&, std::size_t)>) override {}
  void async_write_some(boost::asio::const_buffer, std::function<void(const boost::system::error_code&, std::size_t)>) override {}
  boost::asio::ip::tcp::endpoint remote_endpoint() override { return _endpoint; }
  boost::asio::ip::tcp::endpoint local_endpoint() override { return {}; }
  bool is_open() override { return true; }
  bool is_reliable() override { return _source.transport != "udp"; }
  void shutdown() override {}
  void close() override {}
  std::string transport_name() const override { return _source.transport; }

 private:
  ExplainSource _source;
  boost::asio::ip::tcp::endpoint _endpoint;
};

// Asks the policy one question on the strand, as the node does, and waits for the answer.
template <typename T, typename Ask>
plugins::Result<T> ask(const std::shared_ptr<Core>& core, Ask question) {
  std::promise<plugins::Result<T>> promise;
  auto future = promise.get_future();
  core->post([&]() { question(core->strand(), [&promise](plugins::Result<T> result) { promise.set_value(std::move(result)); }); });
  return future.get();
}

std::string seconds(std::uint32_t value) { return std::to_string(value) + " s"; }

std::string describe(const policy::AuthDecision& decision) {
  switch (decision.kind) {
    case policy::AuthDecision::Kind::Accept:
      return "accept: nothing to prove";
    case policy::AuthDecision::Kind::Trusted:
      return "trusted as trunk " + decision.trunk + ", by the address it came from";
    case policy::AuthDecision::Kind::Digest:
      return std::string("digest: challenged for ") + (decision.realm ? "realm " + decision.realm->name : "any realm here") +
             (decision.from_must_match ? ", the credentials must be the From's" : ", any subscriber's credentials") + "; what follows assumes they are good";
    case policy::AuthDecision::Kind::Reject:
      return "reject: " + std::to_string(decision.code) + " " + decision.reason;
  }
  return {};
}

std::string describe(const policy::RegisterDecision& decision) {
  switch (decision.kind) {
    case policy::RegisterDecision::Kind::Accept:
      return "accept in realm " + (decision.realm ? decision.realm->name : std::string("(none)")) + ", up to " + seconds(decision.max_expires) +
             (decision.min_expires ? ", at least " + seconds(decision.min_expires) : std::string()) +
             (decision.qualify_interval ? ", qualified every " + seconds(decision.qualify_interval) : std::string()) + "; the subscriber is challenged first";
    case policy::RegisterDecision::Kind::Forward:
      return "forward to the domain's own registrar (RFC 3261 10.3 step 1)";
    case policy::RegisterDecision::Kind::Reject:
      return "reject: " + std::to_string(decision.code) + " " + decision.reason;
  }
  return {};
}

std::string describe(const policy::HeaderEdit& edit) {
  switch (edit.op) {
    case policy::HeaderEdit::Op::Set:
      return "set " + edit.name + ": " + edit.value;
    case policy::HeaderEdit::Op::Add:
      return "add " + edit.name + ": " + edit.value;
    case policy::HeaderEdit::Op::Remove:
      return "remove " + edit.name;
    case policy::HeaderEdit::Op::From: {
      std::string parts;
      if (edit.display) parts += " display \"" + *edit.display + "\"";
      if (edit.user) parts += " user " + *edit.user;
      if (edit.host) parts += " host " + *edit.host;
      return "From:" + parts;
    }
  }
  return {};
}

void describe_target(std::ostream& out, const std::shared_ptr<Core>& core, std::size_t number, const policy::Target& target) {
  out << "  " << number << ". ";
  if (target.kind == policy::Target::Kind::Subscriber) {
    out << "subscriber " << (target.subscriber && target.subscriber->identity && target.subscriber->identity->uri ? target.subscriber->identity->uri->to_string() : std::string("(none)"));

    auto bindings = target.bindings;
    if (!bindings && target.subscriber) {
      const auto id = target.subscriber->id;
      const auto read = ask<std::vector<types::Location>>(
          core, [&core, id](plugins::Executor, plugins::Handler<std::vector<types::Location>> handler) { core->location_list(id, std::move(handler)); });
      if (read.ok) bindings = read.value;
    }

    if (!bindings) {
      out << ", its bindings could not be read";
    } else if (bindings->empty()) {
      out << ", no bindings: 480 if nothing else answers";
    } else {
      out << ", " << bindings->size() << (bindings->size() == 1 ? " binding:" : " bindings, newest first:");
      for (const auto& binding : *bindings) {
        out << "\n       " << (binding.contact ? binding.contact->to_string() : std::string("(no contact)"));
        if (!binding.node_id.empty() && binding.node_id != core->config->sip_node_id) out << " through node " << binding.node_id;
      }
    }
  } else {
    out << (target.uri ? target.uri->to_string() : std::string("(no URI)"));
    if (target.next_hop) out << " by " << target.next_hop->to_string();
    if (!target.trunk.empty()) out << ", out by trunk " << target.trunk << ", whose challenges the node answers";
  }
  if (target.ring_timeout) out << ", ringing at most " << target.ring_timeout->count() << " s";
  out << "\n";
}

}  // namespace

std::optional<ExplainSource> parse_explain_source(const std::string& text) {
  ExplainSource source;
  auto rest = text;

  if (const auto colon = rest.find(':'); colon != std::string::npos) {
    const auto first = Util::to_lower(rest.substr(0, colon));
    if (first == "udp" || first == "tcp" || first == "tls" || first == "ws" || first == "wss") {
      source.transport = first;
      rest = rest.substr(colon + 1);
    }
  }

  // An IPv6 address keeps its colons; only a bracketed one takes a port.
  std::string port;
  if (!rest.empty() && rest.front() == '[') {
    const auto close = rest.find(']');
    if (close == std::string::npos) return std::nullopt;
    if (close + 1 < rest.size()) {
      if (rest[close + 1] != ':') return std::nullopt;
      port = rest.substr(close + 2);
    }
    rest = rest.substr(1, close - 1);
  } else if (std::count(rest.begin(), rest.end(), ':') == 1) {
    port = rest.substr(rest.find(':') + 1);
    rest = rest.substr(0, rest.find(':'));
  }

  boost::system::error_code bad;
  boost::asio::ip::make_address(rest, bad);
  if (bad) return std::nullopt;
  source.address = rest;

  if (!port.empty()) {
    try {
      const auto value = std::stoul(port);
      if (value == 0 || value > 65535) return std::nullopt;
      source.port = static_cast<std::uint16_t>(value);
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }
  return source;
}

Explained explain(const std::shared_ptr<loggers::Logger>& logger, const std::shared_ptr<Core>& core, const std::string& request, const ExplainSource& source) {
  Explained explained;
  std::ostringstream out;

  // A file written by hand has bare line feeds; SIP has CRLF, and a blank line before any body.
  std::string text;
  for (std::size_t i = 0; i < request.size(); ++i) {
    if (request[i] == '\n' && (i == 0 || request[i - 1] != '\r')) text += '\r';
    text += request[i];
  }
  if (text.find("\r\n\r\n") == std::string::npos) text += text.size() >= 2 && text.compare(text.size() - 2, 2, "\r\n") == 0 ? "\r\n" : "\r\n\r\n";

  // The header section alone to the parser, as the read loop hands it over.
  const auto split = text.find("\r\n\r\n");
  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>(text.substr(0, split));
  message->body = text.substr(split + 4);
  message->body_length = static_cast<unsigned int>(message->body.size());
  const auto& header = *message->header;
  if (!header.is_valid() || header.type != SIPHeader::Request || !header.request_uri) {
    explained.text = "Not a SIP request this node can read: " + header.summary() + "\n";
    return explained;
  }

  auto channel = std::make_shared<Channel>(logger, core, std::make_shared<StandIn>(source));
  message->channel = channel;

  const auto policy = core->policy();
  const auto fingerprint = policy->fingerprint();
  const auto from = header.contains("From") ? header.headers_map.at("From").front()->to_string() : std::string("(no From)");

  out << "Request    " << header.request_method << " " << header.request_uri->to_string() << "\n";
  out << "From       " << from << "\n";
  out << "Source     " << source.transport << " " << source.address << ":" << source.port << "\n";
  out << "Policy     " << policy->describe() << (fingerprint.empty() ? "" : ", scripts " + fingerprint.substr(0, 12)) << "\n\n";

  const auto& method = header.request_method;
  auto view = std::make_shared<policy::RequestView>();
  view->message = message;
  view->has_route = header.contains("Route");

  // What the node routes without asking (RFC 3261 12.2, 16.4).
  if (method == "ACK" || method == "CANCEL") {
    out << "The node matches " << method << " to its transaction or dialog and asks the policy nothing.\n";
    explained.ok = true;
    explained.text = out.str();
    return explained;
  }
  if (view->has_route) {
    out << "It has a Route: the node follows it once its own entries are taken off, and asks the policy nothing.\n";
    explained.ok = true;
    explained.text = out.str();
    return explained;
  }
  const auto to = header.contains("To") ? header.headers_map.at("To").front()->as<headers::SIPIdentityHeader>() : nullptr;
  if (method != "REGISTER" && to && to->value && to->value->tags.count("tag")) {
    out << "Its To has a tag: inside a dialog, routed by the dialog if this node knows it, and the policy is not asked.\n";
    explained.ok = true;
    explained.text = out.str();
    return explained;
  }

  if (method == "REGISTER") {
    const auto decided = ask<policy::RegisterDecision>(
        core, [&](plugins::Executor on, plugins::Handler<policy::RegisterDecision> handler) { policy->register_(on, view, std::move(handler)); });
    if (!decided.ok) {
      out << "register   failed - " << decided.error << "\n           the node answers 500\n";
      explained.text = out.str();
      return explained;
    }
    out << "register   " << describe(decided.value) << "\n";
    if (decided.value.kind != policy::RegisterDecision::Kind::Forward) {
      explained.ok = true;
      explained.text = out.str();
      return explained;
    }
    view->relay = true;
  }

  const auto authorised = ask<policy::AuthDecision>(
      core, [&](plugins::Executor on, plugins::Handler<policy::AuthDecision> handler) { policy->authorize(on, view, std::move(handler)); });
  if (!authorised.ok) {
    out << "authorize  failed - " << authorised.error << "\n           the node answers 500\n";
    explained.text = out.str();
    return explained;
  }
  out << "authorize  " << describe(authorised.value) << "\n";
  if (authorised.value.kind == policy::AuthDecision::Kind::Reject || view->relay) {
    explained.ok = true;
    explained.text = out.str();
    return explained;
  }
  if (authorised.value.kind == policy::AuthDecision::Kind::Trusted) view->trunk = authorised.value.trunk;

  const auto routed = ask<policy::RouteDecision>(
      core, [&](plugins::Executor on, plugins::Handler<policy::RouteDecision> handler) { policy->route(on, view, std::move(handler)); });
  if (!routed.ok) {
    out << "route      failed - " << routed.error << "\n           the node answers 500\n";
    explained.text = out.str();
    return explained;
  }

  const auto& route = routed.value;
  if (route.kind == policy::RouteDecision::Kind::Reply) {
    out << "route      reply " << route.code << " " << route.reason << "\n";
  } else if (route.targets.empty()) {
    out << "route      forward to nothing: 480 Temporarily Unavailable\n";
  } else {
    out << "route      forward, trying in turn:\n";
    for (std::size_t i = 0; i < route.targets.size(); ++i) describe_target(out, core, i + 1, route.targets[i]);
    if (route.media) {
      out << "           media " << (route.media->anchor ? "anchored" : "end to end") << ", profile " << types::MediaPolicy::to_string(route.media->profiles)
          << "\n";
    }
    if (route.rewrite_contact) out << "           Contact " << (*route.rewrite_contact ? "rewritten" : "left alone") << "\n";
    for (const auto& edit : view->edits) out << "           on what is forwarded: " << describe(edit) << "\n";
  }

  explained.ok = true;
  explained.text = out.str();
  return explained;
}

}  // namespace athenasip::cli
