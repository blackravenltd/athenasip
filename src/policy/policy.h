//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../loggers/logger.h"
#include "../plugins/plugin.h"
#include "../plugins/plugin_registry.h"
#include "../sip_message.h"
#include "../types/location.h"
#include "../types/realm.h"
#include "../types/sip_identity.h"
#include "../types/subscriber.h"
#include "../types/trunk.h"

namespace athenasip {
class Config;
}

namespace athenasip::policy {

inline constexpr char kind[] = "policy";

// A change to the copies the node forwards, never to the request as it arrived. The headers that carry the
// transaction, the dialog and the route are not for a policy to touch (HeaderEdit::guarded).
struct HeaderEdit {
  enum class Op {
    Set,     // every value of the header replaced by this one
    Add,     // one more value
    Remove,  // every value gone
    From,    // the From's display name, user or host; never its tag
  };

  Op op = Op::Set;
  std::string name;
  std::string value;

  // From.
  std::optional<std::string> display;
  std::optional<std::string> user;
  std::optional<std::string> host;

  // Whether a header is one a policy may not edit: Via, Route, Record-Route, Path, CSeq, Call-ID, Max-Forwards,
  // Content-Length, Content-Type, Contact, From (through Op::From only) and To, by their names and compact forms.
  static bool guarded(const std::string& name) {
    std::string lowered;
    for (const char c : name) lowered.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
    for (const char* forbidden : {"via", "v", "route", "record-route", "path", "cseq", "call-id", "i", "max-forwards", "content-length", "l", "content-type",
                                  "c", "contact", "m", "from", "f", "to", "t"}) {
      if (lowered == forbidden) return true;
    }
    return false;
  }
};

// A request as the policy sees it: the message, and what the node knows about it that the message does not
// say. The node acts on a policy's answer; a policy never sends anything.
struct RequestView {
  std::shared_ptr<SIPMessage> message;

  // A REGISTER for a domain this node does not serve, which the node would forward (RFC 3261 10.3 step 1).
  bool relay = false;

  // Arrived over the inter-node listener, from a client certificate under the cluster CA.
  bool from_peer = false;

  // Carries a flow token this node sealed, in a Route it wrote as Record-Route or Path.
  bool valid_flow_token = false;

  // A Route header remains once this node's own entries are taken off (RFC 3261 16.4).
  bool has_route = false;

  // After authorize: the trunk the policy trusted the request as coming from, by name; empty otherwise.
  std::string trunk;

  // Made by route, applied to every copy forwarded.
  std::vector<HeaderEdit> edits;
};

// How the caller of an out-of-dialog request is to be proved. The node challenges and verifies; the policy only
// says which proof applies.
struct AuthDecision {
  enum class Kind {
    Accept,   // already proved, or nothing to prove
    Digest,   // RFC 3261 22.3 in a realm this node serves
    Trusted,  // from a trunk, proved by where it came from; `trunk` names it
    Reject,
  };

  Kind kind = Kind::Reject;

  // Digest: the realm. Null means any realm this node serves, with one challenge for each.
  std::shared_ptr<types::Realm> realm;

  // Digest: the credentials must be for the subscriber the From names. Without it, a reliable flow any
  // subscriber registered over is trusted, and credentials for any subscriber pass.
  bool from_must_match = true;

  // Trusted.
  std::string trunk;

  // Reject.
  std::uint16_t code = 403;
  std::string reason = "Forbidden";

  static AuthDecision accept() { return AuthDecision{Kind::Accept}; }
  static AuthDecision trusted(std::string trunk) {
    AuthDecision decision{Kind::Trusted};
    decision.trunk = std::move(trunk);
    return decision;
  }
  static AuthDecision digest(std::shared_ptr<types::Realm> realm, bool from_must_match = true) {
    AuthDecision decision{Kind::Digest};
    decision.realm = std::move(realm);
    decision.from_must_match = from_must_match;
    return decision;
  }
  static AuthDecision reject(std::uint16_t code, std::string reason) {
    AuthDecision decision{Kind::Reject};
    decision.code = code;
    decision.reason = std::move(reason);
    return decision;
  }
};

// One entry of a route decision. The node expands it into the branches it forwards to.
struct Target {
  enum class Kind {
    Subscriber,  // every binding of an address of record, in the node's order, by push or a peer node as needed
    Uri,         // one URI, located by RFC 3263
  };

  Kind kind = Kind::Uri;

  // Subscriber. The bindings when the policy has read them; the node reads them when absent.
  std::shared_ptr<types::Subscriber> subscriber;
  std::optional<std::vector<types::Location>> bindings;

  // Uri: the Request-URI to send, and where to send it when that differs.
  std::shared_ptr<types::SIPUri> uri;
  std::shared_ptr<types::SIPUri> next_hop;

  // Uri: the trunk this leaves by, by name. The node answers its challenges with the trunk's credentials.
  std::string trunk;

  // How long an INVITE branch may ring before the node gives up on it and moves on: a CANCEL if it rang, a 408 to
  // the policy either way. Unset is timer C (RFC 3261 16.8), minutes.
  std::optional<std::chrono::seconds> ring_timeout;

  static Target of(std::shared_ptr<types::Subscriber> subscriber, std::optional<std::vector<types::Location>> bindings = std::nullopt) {
    Target target{Kind::Subscriber};
    target.subscriber = std::move(subscriber);
    target.bindings = std::move(bindings);
    return target;
  }
  static Target to(std::shared_ptr<types::SIPUri> uri, std::shared_ptr<types::SIPUri> next_hop = nullptr, std::string trunk = {}) {
    Target target{Kind::Uri};
    target.uri = std::move(uri);
    target.next_hop = std::move(next_hop);
    target.trunk = std::move(trunk);
    return target;
  }
};

// Where an authorised initial request goes: the targets to try in turn, or a final response instead.
struct RouteDecision {
  enum class Kind {
    Forward,
    Reply,
  };

  Kind kind = Kind::Reply;

  // Forward. With no targets the node answers 480.
  std::vector<Target> targets;

  // Forward: the call's media policy and whether Contacts are rewritten; the node's defaults when absent.
  std::optional<types::MediaPolicy> media;
  std::optional<bool> rewrite_contact;

  // Reply.
  std::uint16_t code = 480;
  std::string reason = "Temporarily Unavailable";

  static RouteDecision forward(std::vector<Target> targets) {
    RouteDecision decision{Kind::Forward};
    decision.targets = std::move(targets);
    return decision;
  }
  static RouteDecision reply(std::uint16_t code, std::string reason) {
    RouteDecision decision{Kind::Reply};
    decision.code = code;
    decision.reason = std::move(reason);
    return decision;
  }
};

// Where a fork stands when a branch has failed: how many targets have been tried and how many are left.
struct ForkState {
  std::size_t tried = 0;
  std::size_t remaining = 0;
  std::uint16_t best = 0;
};

// What to do after a branch ends in 3xx to 5xx. 2xx and 6xx end a fork whatever a policy says (RFC 3261 16.7).
struct FailureDecision {
  enum class Kind {
    Next,  // try the next target
    Stop,  // try nothing more; the best response so far goes to the caller
  };

  Kind kind = Kind::Next;

  // Next: tried before the targets that remain.
  std::vector<Target> first;

  static FailureDecision next(std::vector<Target> first = {}) {
    FailureDecision decision{Kind::Next};
    decision.first = std::move(first);
    return decision;
  }
  static FailureDecision stop() { return FailureDecision{Kind::Stop}; }
};

// What a registrar does with a REGISTER: register it in a realm, forward it, or refuse it.
struct RegisterDecision {
  enum class Kind {
    Accept,
    Forward,  // RFC 3261 10.3 step 1
    Reject,
  };

  Kind kind = Kind::Reject;

  // Accept: the realm the address of record is in, which authenticates it.
  std::shared_ptr<types::Realm> realm;

  // Accept: the longest lifetime granted, and the shortest accepted (0 for none, RFC 3261 10.3 step 7).
  std::uint32_t max_expires = 3600;
  std::uint32_t min_expires = 0;

  // Accept: seconds between OPTIONS to each binding; 0 for none.
  std::uint32_t qualify_interval = 0;

  // Reject.
  std::uint16_t code = 403;
  std::string reason = "Forbidden";

  static RegisterDecision accept(std::shared_ptr<types::Realm> realm, std::uint32_t max_expires, std::uint32_t min_expires, std::uint32_t qualify_interval) {
    RegisterDecision decision{Kind::Accept};
    decision.realm = std::move(realm);
    decision.max_expires = max_expires;
    decision.min_expires = min_expires;
    decision.qualify_interval = qualify_interval;
    return decision;
  }
  static RegisterDecision forward() { return RegisterDecision{Kind::Forward}; }
  static RegisterDecision reject(std::uint16_t code, std::string reason) {
    RegisterDecision decision{Kind::Reject};
    decision.code = code;
    decision.reason = std::move(reason);
    return decision;
  }
};

// What the node lends a policy to decide with. Handlers run on the Core strand.
class Host {
 public:
  virtual ~Host() = default;

  virtual void realm(std::string name, plugins::Handler<std::shared_ptr<types::Realm>> handler) = 0;
  virtual void subscriber(std::shared_ptr<types::SIPIdentity> identity, plugins::Handler<std::shared_ptr<types::Subscriber>> handler) = 0;
  virtual void locations(std::uint64_t subscriber_id, plugins::Handler<std::vector<types::Location>> handler) = 0;
  virtual void trunk(std::string name, plugins::Handler<std::shared_ptr<types::Trunk>> handler) = 0;
  virtual void trunks(plugins::Handler<std::vector<std::shared_ptr<types::Trunk>>> handler) = 0;

  // Whether a host and port are one of this node's own addresses.
  virtual bool names_this_node(const std::string& host, std::uint16_t port) const = 0;

  virtual const Config& config() const = 0;
};

// Routing and authorisation policy: who a caller is, where a request goes, what follows a failed branch, and
// what becomes of a REGISTER. The node asks at each of those points and carries out the answer. The SIP
// mechanics - transactions, Via, Record-Route, flow tokens, Digest, media - stay the node's.
//
// A failed Result is a policy that could not decide (a store it read failed, a script raised): the node
// answers 500, except after a failed branch, where it goes on to the next target.
class Policy : public plugins::Plugin {
 public:
  std::string kind() const final { return policy::kind; }

  // Called once, before the first question. The host outlives the policy.
  virtual void attach(std::shared_ptr<Host> host) { _host = std::move(host); }

  // An out-of-dialog request reaching the proxy, or a REGISTER to relay (view->relay).
  virtual void authorize(plugins::Executor on, std::shared_ptr<RequestView> request, plugins::Handler<AuthDecision> handler) = 0;

  // An authorised request with no Route and no flow token left to follow.
  virtual void route(plugins::Executor on, std::shared_ptr<RequestView> request, plugins::Handler<RouteDecision> handler) = 0;

  // A branch has ended in 3xx to 5xx and the fork goes on.
  virtual void on_failure(plugins::Executor on, std::shared_ptr<RequestView> request, std::shared_ptr<SIPMessage> response, ForkState state,
                          plugins::Handler<FailureDecision> handler) {
    (void)request;
    (void)response;
    (void)state;
    _complete(std::move(on), std::move(handler), plugins::Result<FailureDecision>::success(FailureDecision::next()));
  }

  // A REGISTER, before it is challenged.
  virtual void register_(plugins::Executor on, std::shared_ptr<RequestView> request, plugins::Handler<RegisterDecision> handler) = 0;

  // Reads its rules again, on the Core strand. Empty on success, else why not, with the rules in force unchanged.
  virtual std::string reload() { return {}; }

  // What identifies the rules in force, for a cluster to see that its nodes agree. Empty when the rules are the
  // driver's own code.
  virtual std::string fingerprint() const { return {}; }

  template <typename T, typename = std::enable_if_t<std::is_base_of_v<Policy, T>>>
  static void register_driver(std::shared_ptr<loggers::Logger> logger, std::string scheme) {
    plugins::PluginRegistry::instance().add<T>(std::move(logger), policy::kind, std::move(scheme));
  }

  static std::shared_ptr<Policy> create_driver(std::shared_ptr<loggers::Logger> logger, const std::string& url_string) {
    return plugins::PluginRegistry::instance().create_as<Policy>(std::move(logger), policy::kind, url_string);
  }

 protected:
  std::shared_ptr<Host> _host;
};

}  // namespace athenasip::policy
