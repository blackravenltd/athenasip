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
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "dns/sip_locator.h"
#include "headers/sip_identity_header.h"
#include "loggers/logger.h"
#include "media/media_engine.h"
#include "sip_message.h"
#include "timer_source.h"
#include "transaction_user.h"
#include "transactions/transaction_base.h"
#include "types/location.h"
#include "types/realm.h"

namespace athenasip {

class Channel;
class Core;

// RFC 3261 section 16 proxy, and the transaction user for everything except REGISTER. Order of work: loop
// detection (16.3.4), route preprocessing (16.4), target determination (16.5), forwarding with serial forking
// (16.6), response processing (16.7) and CANCEL (16.10).
//
// Transaction-stateful, not dialog-stateful (16.1): in-dialog requests are routed by the Route set that
// Record-Route established. Dialog tracking belongs to the layer above.
class Proxy : public TransactionUser {
 public:
  Proxy(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Core> core);

  // RFC 3261 10.3 step 1: forwards a REGISTER for a domain this node does not serve, for one of this node's
  // subscribers only, with this node on the Path (RFC 3327, RFC 5626 5.1).
  void forward_register(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction);

  // The registrar has just stored this contact for a subscriber. A request waiting for that client's push is
  // forwarded now rather than at the next poll.
  void on_registered(std::uint64_t subscriber_id, const std::shared_ptr<SIPUri>& contact);

  void on_request(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction) override;

  // RFC 3261 9.2, 16.10: answers the CANCEL 200 and the INVITE it names 487, and cancels the branch in
  // flight.
  void on_cancel(std::shared_ptr<SIPMessage> cancel, std::shared_ptr<transactions::TransactionBase> cancel_transaction,
                 std::shared_ptr<transactions::TransactionBase> invite_transaction);

  // RFC 3261 16.7 step 1, 18.1.2: a response matching no client transaction is forwarded statelessly down the
  // Via chain.
  void on_stray_response(std::shared_ptr<SIPMessage> response);

 private:
  // Transport, host and port of a hop URI (RFC 3261 16.6 step 7).
  struct NextHop {
    std::string transport;
    std::string host;
    std::uint16_t port = 5060;
  };

  // One entry of the target set (16.5). `uri` becomes the Request-URI; `next_hop` is where the request is
  // sent, which differs when a Route set is in play.
  struct Target {
    std::shared_ptr<SIPUri> uri;
    std::shared_ptr<SIPUri> next_hop;
    std::weak_ptr<Channel> flow;

    // Set on the re-offer after a 488: the profile to offer, and the one the target refused.
    std::optional<media::Profile> profile;
    std::optional<media::Profile> rejected;

    // The media profile the client on this flow reported when last qualified.
    std::optional<media::Profile> said;

    // RFC 5626: the client instance owning this flow, empty for an ordinary binding. `dead` means the flow
    // has gone, which fails the target; its Contact is not tried.
    std::string instance;
    bool dead = false;

    // RFC 8599: a binding this node pushes to before forwarding. The pn-* parameters are on its contact.
    std::optional<types::Location> push;
  };

  // The response context (16.7). Kept alive by the client transaction callbacks.
  struct Context {
    std::shared_ptr<SIPMessage> request;
    std::shared_ptr<transactions::TransactionBase> server;

    // The 16.3.4 loop token, computed once from the request as received and shared by every branch.
    std::string loop_token;

    // The 2xx as sent to the caller, resent when the callee retransmits it (RFC 6026 8.4).
    std::shared_ptr<SIPMessage> answer_sent;

    std::vector<Target> targets;
    std::size_t next = 0;

    std::shared_ptr<SIPMessage> best;

    // The branch in flight. RFC 3261 9.1 builds the CANCEL from the request as sent.
    std::shared_ptr<SIPMessage> forwarded;
    std::weak_ptr<Channel> forwarded_flow;

    // RFC 3261 9.1: a CANCEL is held until the branch has answered provisionally.
    bool provisional = false;
    bool cancelled = false;

    // RFC 4028 8.1: whether the request supported "timer", and the interval forwarded. 8.2 needs both.
    bool session_timer_supported = false;
    std::uint32_t session_interval = 0;

    // RFC 3261 16.6 step 11: timer C and the client transaction it bounds. Forking is serial, so one of each.
    std::shared_ptr<Timer> timer_c;
    std::shared_ptr<transactions::TransactionBase> client;

    // Timer C has fired once and a CANCEL has gone out; the branch gets one more interval (16.8).
    bool timer_c_cancelled = false;

    // A final response has gone upstream; late branch answers are not forwarded.
    bool answered = false;

    // The realm's media policy, when target determination found a realm. In-dialog requests use the call's.
    std::optional<types::MediaPolicy> media_policy;

    // The callee subscriber's media profile.
    std::optional<types::MediaPolicy::Profiles> callee_profile;

    // Whether Contacts are rewritten to where messages came from (types::Behaviour).
    bool rewrite_contact = false;

    // The caller subscriber's media profile, read only for an INVITE without SDP (RFC 3264 section 5).
    std::optional<types::MediaPolicy::Profiles> caller_profile;

    // The target of the branch in flight, and the profile offered to it when the callee had not stated one. A
    // 488 to that offer earns one re-offer.
    Target current;
    std::optional<media::Profile> offered;

    // RFC 5626 5.3: each instance's remaining flows, best first, tried when the one in the target set fails.
    std::map<std::string, std::vector<Target>> other_flows;

    // RFC 3263 4.3: the remaining DNS hops for the current target, tried on a 503 or a timeout before the
    // fork moves on.
    std::vector<dns::Hop> hops_left;
    std::optional<Target> hop_target;

    // RFC 8599 5.2: the target waiting in the push bucket for its client to re-register, the bucket timer's
    // deadline, and the poll of the store.
    std::optional<Target> pushed;
    std::chrono::steady_clock::time_point push_deadline;
    std::shared_ptr<Timer> push_poll;
  };

  // on_request after the 16.3 checks and OPTIONS for this node: session timer, routes, authorization, targets.
  void _proceed(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction, const std::string& token);

  // RFC 3261 11.2: this node's answer to an OPTIONS for itself.
  void _answer_options(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request);

  // RFC 4028 8.1: enforces the minimum session interval. False when the request was answered 422.
  bool _apply_session_timer(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction);

  // RFC 4028 8.1: adds this node's Session-Expires to a request that carries none.
  void _insert_session_timer(const std::shared_ptr<SIPMessage>& request);

  // RFC 4028 8.2: adds Session-Expires to a 2xx when the caller asked for a timer and the callee answered
  // without one.
  void _complete_session_timer(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& response);

  // 422 with the Min-SE that RFC 4028 section 6 requires.
  void _send_interval_too_small(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                                std::uint32_t minimum);

  // RFC 3261 16.3.4: a hash of the fields that decide routing, carried in the branch. The same token in one
  // of this node's Vias is a loop; a different one is a spiral.
  std::string _loop_token(const std::shared_ptr<SIPMessage>& request) const;
  bool _is_loop(const std::shared_ptr<SIPMessage>& request, const std::string& token) const;

  // RFC 3261 16.4: undoes a strict router's rewrite and removes Routes naming this node.
  void _preprocess_routes(const std::shared_ptr<SIPMessage>& request) const;

  // RFC 3261 16.5: fills a context and calls _forward_next, or answers the caller when there is no target.
  void _determine_targets(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction,
                          const std::string& loop_token);

  // RFC 3261 16.6 steps 2, 3, 4, 6 and 8: Request-URI, Max-Forwards, Record-Route, route set and Via. False
  // when Max-Forwards is exhausted.
  bool _prepare_forward(const std::shared_ptr<SIPMessage>& copy, const std::shared_ptr<Channel>& channel, const Target& target,
                        const std::string& loop_token) const;

  // Forwards to the next untried target, opening a flow if needed; sends the best response when none are
  // left.
  void _forward_next(const std::shared_ptr<Context>& context);
  void _forward_target(const std::shared_ptr<Context>& context, const Target& target);

  // RFC 8599 5.6.2: pushes to a target's client and holds the request until it re-registers. A failed push or
  // the bucket timer is a 480 for that target, and the fork moves on.
  void _push_and_wait(const std::shared_ptr<Context>& context, const Target& target);
  void _push_poll(const std::shared_ptr<Context>& context);
  void _push_check(const std::shared_ptr<Context>& context, const std::string& registered);
  void _connect_hops(const std::shared_ptr<Context>& context, const Target& target, std::vector<dns::Hop> hops, std::size_t index);
  void _unreachable(const std::shared_ptr<Context>& context);
  bool _try_next_hop(const std::shared_ptr<Context>& context);

  // Copies the request, prepares it for the target and hands it to the media engine.
  void _forward_to(const std::shared_ptr<Context>& context, const Target& target, const std::shared_ptr<Channel>& channel);

  // Runs once the media engine has handled the SDP. Moves an oversized request off UDP (RFC 3261 18.1.1).
  void _send_forward(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& copy, const std::shared_ptr<Channel>& channel);

  // Starts the client transaction, or writes an ACK straight to the transport.
  void _write_forward(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& copy, const std::shared_ptr<Channel>& channel);

  void _on_response(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& response);

  // RFC 3261 16.7: sends a response to the caller once the media engine has handled its SDP.
  void _forward_response(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& response);

  // Passes a session description through the media engine (RFC 3264) so that media is relayed through this
  // node. This departs from RFC 3261 16.6, which leaves bodies alone; with no engine, no call, or an engine
  // that declines, the message is forwarded unchanged.
  //
  // `then` runs when the message is ready: inline if there was nothing to do, otherwise on the strand.
  void _anchor_media(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<SIPMessage>& message, const std::shared_ptr<Channel>& outgoing,
                     const std::shared_ptr<Context>& context, std::function<void()> then);

  // RFC 3261 16.7 step 6: sends the best response once every branch has been tried.
  void _send_best(const std::shared_ptr<Context>& context);
  void _rewrite_contact(const std::shared_ptr<SIPMessage>& message, const std::shared_ptr<Channel>& from) const;
  void _read_caller_profile(const std::shared_ptr<Context>& context, std::function<void()> then);
  bool _reoffer(const std::shared_ptr<Context>& context);
  void _add_targets(const std::shared_ptr<Context>& context, std::vector<types::Location> bindings) const;
  bool _try_other_flow(const std::shared_ptr<Context>& context);
  void _report_reoffer(const std::shared_ptr<Context>& context, bool took);

  // RFC 3261 16.10: sends the CANCEL for the branch in flight.
  void _cancel_branch(const std::shared_ptr<Context>& context);

  // RFC 3261 16.6 step 11, 16.7 step 2, 16.8: timer C bounds an INVITE branch that keeps answering
  // provisionally. Timer B stops at the first provisional (17.1.1.2), so nothing else does.
  void _timer_c_start(const std::shared_ptr<Context>& context);
  void _timer_c_cancel(const std::shared_ptr<Context>& context);
  void _on_timer_c(const std::shared_ptr<Context>& context);

  // RFC 3261 16.10: forwards a CANCEL that has no response context.
  void _forward_cancel_statelessly(const std::shared_ptr<SIPMessage>& cancel, const std::shared_ptr<transactions::TransactionBase>& transaction);

  void _send_status(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request, std::uint16_t code,
                    const std::string& reason);

  // Decides whether the request may be forwarded (RFC 3261 22.3). `then` runs only if it may; otherwise the
  // request has been answered.
  void _authorize(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction, std::function<void()> then);
  void _authenticate(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction,
                     const std::shared_ptr<types::Realm>& realm, const std::shared_ptr<SIPUri>& caller, std::function<void()> then);
  void _send_proxy_challenge(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                             const std::shared_ptr<types::Realm>& realm);

  // Who may have a REGISTER forwarded: a reliable connection one of this node's subscribers registered over, or
  // credentials for one of its realms (RFC 3261 22.3). The From is the foreign address of record, so it does not
  // decide.
  void _authorize_relay(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction,
                        std::function<void()> then);
  void _send_relay_challenge(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request);

  // This node as the side facing `facing` sees it, with a flow token in the user part (RFC 5626 5.1): for
  // Record-Route and Path.
  std::shared_ptr<headers::SIPIdentityHeader> _route_to_this_node(Core& core, const std::string& transport, const Channel& facing, const std::string& token,
                                                                  bool secure) const;

  bool _names_this_node(const SIPUri& uri) const;
  std::shared_ptr<Channel> _flow_to(const SIPUri& uri) const;

  // RFC 5626: the target for a binding, preferring the flow it registered over. A forgotten UDP flow is still
  // used; a closed reliable one falls back to the Contact, or fails the target for an outbound binding.
  Target _target_for(const types::Location& binding) const;

  // Cluster forwarding: whether another node holds a binding's flow, and that node as a target.
  bool _held_elsewhere(Core& core, const types::Location& binding) const;
  std::optional<Target> _peer_target(Core& core, const std::string& node_id, const std::shared_ptr<SIPMessage>& request) const;
  static std::shared_ptr<SIPUri> _datagram_hop(const std::string& flow_id);

  static NextHop _next_hop_of(const SIPUri& uri);

  // Maps the policy's profile setting to an engine profile. Only FromTransport consults the transport.
  static media::Flags::Profile _profile_under(const types::MediaPolicy& policy, const std::string& transport);

  std::shared_ptr<loggers::Logger> _logger;
  std::weak_ptr<Core> _core;

  // Response contexts by server transaction id, so a CANCEL can find its INVITE's branch. Weak: a context is
  // owned by the callbacks in flight.
  std::unordered_map<std::string, std::weak_ptr<Context>> _contexts;

  // Contexts with a request in the push bucket.
  std::vector<std::weak_ptr<Context>> _push_waiting;
};

}  // namespace athenasip
