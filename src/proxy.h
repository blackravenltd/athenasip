//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

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

// RFC 3261 section 16: the proxy, and the transaction user for everything that is not a
// REGISTER.
//
// The order of work is the RFC's own: loop detection (16.3.4), route preprocessing
// (16.4), target determination (16.5), forwarding (16.6) with serial forking, response
// processing (16.7) and CANCEL (16.10).
//
// A proxy is transaction-stateful and not dialog-stateful (16.1). Nothing here knows
// what a dialog is: an in-dialog request reaches its far end because the Route set the
// endpoints kept from the Record-Route brings it back through this node, and because
// its Request-URI is a target this node is not responsible for and so is forwarded as
// it stands. Dialog tracking is a separate concern and belongs to the layer above.
class Proxy : public TransactionUser {
 public:
  Proxy(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Core> core);

  void on_request(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction) override;

  // RFC 3261 9.2 and 16.10: a CANCEL is answered 200 OK on its own transaction, the
  // INVITE server transaction it names is answered 487, and the branch already tried is
  // cancelled in turn.
  void on_cancel(std::shared_ptr<SIPMessage> cancel, std::shared_ptr<transactions::TransactionBase> cancel_transaction,
                 std::shared_ptr<transactions::TransactionBase> invite_transaction);

  // RFC 3261 16.7 step 1 and 18.1.2: a response matching no client transaction has no
  // context to be the best response of. It goes back down the Via chain with this
  // node's own Via removed, and nothing is remembered about it.
  void on_stray_response(std::shared_ptr<SIPMessage> response);

 private:
  // Where a hop actually goes, once its URI has given up its transport, host and port
  // (RFC 3261 16.6 step 7).
  struct NextHop {
    std::string transport;
    std::string host;
    std::uint16_t port = 5060;
  };

  // One entry of the target set (16.5). The URI that becomes the Request-URI and the
  // hop the request is handed to are not the same thing whenever a Route set is in
  // play: the Request-URI stays as it arrived and the top Route is where it goes.
  struct Target {
    std::shared_ptr<SIPUri> uri;
    std::shared_ptr<SIPUri> next_hop;
    std::weak_ptr<Channel> flow;

    // Set on the one re-offer a 488 earns: the profile to offer this time, and the one the
    // target refused.
    std::optional<media::Profile> profile;
    std::optional<media::Profile> rejected;

    // What the client on this target's flow said its media is when last qualified.
    std::optional<media::Profile> said;

    // RFC 5626 outbound: the client instance this flow belongs to, empty for an ordinary
    // binding, and whether the flow has gone - which for an outbound binding is the target
    // failing, never a reason to try its Contact.
    std::string instance;
    bool dead = false;
  };

  // The response context (16.7): the request as received, the server transaction it
  // arrived on, the targets left to try, and the best response so far. It is kept alive
  // by the callbacks the client transactions hold, and dies with the last of them.
  struct Context {
    std::shared_ptr<SIPMessage> request;
    std::shared_ptr<transactions::TransactionBase> server;

    // The 16.3.4 half of the branch this node writes, computed once from the request as
    // it arrived so that every branch of the fork carries the same one.
    std::string loop_token;

    std::vector<Target> targets;
    std::size_t next = 0;

    std::shared_ptr<SIPMessage> best;

    // The branch in flight, for CANCEL (16.10). RFC 3261 9.1 builds the CANCEL from the
    // request that was sent, not from the one that arrived.
    std::shared_ptr<SIPMessage> forwarded;
    std::weak_ptr<Channel> forwarded_flow;

    // A CANCEL may not go out before a provisional response has come back (9.1), so one
    // that arrives early is held until the branch answers.
    bool provisional = false;
    bool cancelled = false;

    // RFC 4028 section 8.1: "The proxy MUST remember, for the duration of the
    // transaction, whether the request contained the Supported header field with the
    // value 'timer'", and the interval it forwarded. Section 8.2 needs both to answer a
    // callee that says nothing about session timers at all.
    bool session_timer_supported = false;
    std::uint32_t session_interval = 0;

    // RFC 3261 16.6 step 11: timer C, and the client transaction it bounds. The fork is
    // serial, so there is one branch in flight and one of each at a time.
    std::shared_ptr<Timer> timer_c;
    std::shared_ptr<transactions::TransactionBase> client;

    // A CANCEL has already gone out because timer C fired once. 16.8 offers a reset of
    // the timer as an alternative to terminating the transaction, so the branch gets one
    // more interval to answer the CANCEL before it is terminated outright.
    bool timer_c_cancelled = false;

    // A final response has gone upstream. The search is over, and a late answer from a
    // branch must not be sent a second time.
    bool answered = false;

    // The realm's media policy, where target determination found a realm to read it
    // from. An in-dialog request has none, and takes the one the call remembers.
    std::optional<types::MediaPolicy> media_policy;

    // The callee's account's media profile, read where the account was in hand.
    std::optional<types::MediaPolicy::Profiles> callee_profile;

    // Whether Contacts are rewritten to where messages came from (types::Behaviour).
    bool rewrite_contact = false;

    // The caller's, read only for an INVITE with no description (RFC 3264 section 5).
    std::optional<types::MediaPolicy::Profiles> caller_profile;

    // The branch in flight's target, and the profile the engine made its offer for when
    // nothing the callee had said decided it. A 488 to that offer is the one refusal this
    // node can do something about.
    Target current;
    std::optional<media::Profile> offered;

    // RFC 5626 section 5.3: one flow per client instance is in the target set at a time,
    // and these are each instance's other flows, best first, for when that one fails.
    std::map<std::string, std::vector<Target>> other_flows;

    // RFC 3263 4.3: the places the current target can still be tried, when DNS listed more
    // than one and the branch in flight went to the first. A 503 or a timeout from that
    // branch is the server failing rather than the call being refused, and the same target
    // goes to the next of these instead of the fork moving on.
    std::vector<dns::Hop> hops_left;
    std::optional<Target> hop_target;
  };

  // RFC 4028 section 8.1: this node's say in the session timer negotiation, applied to
  // the request before any copy of it is forwarded. False when the request was answered
  // 422 and must go no further.
  bool _apply_session_timer(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction);

  // RFC 4028 section 8.1's other half: the interval this node puts on a request that
  // asked for none, so that a call whose far end knows what a session timer is gets one
  // whether or not the near end thought to ask.
  void _insert_session_timer(const std::shared_ptr<SIPMessage>& request);

  // RFC 4028 section 8.2: the 2xx for a session refresh request whose caller asked for a
  // timer and whose callee answered without one.
  void _complete_session_timer(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& response);

  // 422 with the Min-SE that RFC 4028 section 6 requires on it.
  void _send_interval_too_small(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                                std::uint32_t minimum);

  // RFC 3261 16.3.4. The branch this node writes carries a hash of the fields that
  // decide where a request goes, so a request that comes back can be told apart from
  // one that never left: same hash is a loop, a different one is a spiral.
  std::string _loop_token(const std::shared_ptr<SIPMessage>& request) const;
  bool _is_loop(const std::shared_ptr<SIPMessage>& request, const std::string& token) const;

  // RFC 3261 16.4. Undoes a strict router's rewrite and takes off a Route naming this
  // node, so that what is left is the route set the request still has to travel.
  void _preprocess_routes(const std::shared_ptr<SIPMessage>& request) const;

  // RFC 3261 16.5, once route preprocessing is done. Answers the caller itself when
  // there is nothing to route to, and otherwise hands a filled context to _forward_next.
  void _determine_targets(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction,
                          const std::string& loop_token);

  // Rewrites one copy of the request for one hop: Request-URI, Max-Forwards,
  // Record-Route, the route set and this node's Via (16.6 steps 2, 3, 4, 6 and 8).
  // False when Max-Forwards has run out.
  bool _prepare_forward(const std::shared_ptr<SIPMessage>& copy, const std::shared_ptr<Channel>& channel, const Target& target,
                        const std::string& loop_token) const;

  // Sends to the next untried target, and answers the caller when there are none left.
  // Opens a flow to the target first where this node has none, which is a round trip, so
  // the sending half is _forward_to.
  void _forward_next(const std::shared_ptr<Context>& context);
  void _connect_hops(const std::shared_ptr<Context>& context, const Target& target, std::vector<dns::Hop> hops, std::size_t index);
  void _unreachable(const std::shared_ptr<Context>& context);
  bool _try_next_hop(const std::shared_ptr<Context>& context);

  // One target, one flow: the copy of the request, the rewrites 16.6 asks for, and the
  // send.
  void _forward_to(const std::shared_ptr<Context>& context, const Target& target, const std::shared_ptr<Channel>& channel);

  // The tail of _forward_next, once the session description has been through the media
  // engine. Separate because that is a round trip and the send waits for it. This is also
  // where RFC 3261 18.1.1 moves an oversized request off UDP, which is another one.
  void _send_forward(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& copy, const std::shared_ptr<Channel>& channel);

  // The send itself, once the transport is settled.
  void _write_forward(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& copy, const std::shared_ptr<Channel>& channel);

  void _on_response(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& response);

  // RFC 3261 16.7: a response on its way back to the caller, once its own session
  // description has been through the media engine.
  void _forward_response(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& response);

  // RFC 3264 through the media engine. A session description on its way through belongs
  // to the leg it came from - a request carries the description of the end that sent it,
  // and a response carries the description of the end that answered - and what the
  // engine gives back names this node instead, so the media arrives here to be bridged
  // rather than going end to end.
  //
  // This is a departure from 16.6, which says a proxy does not add to, modify or remove
  // a body. The node does it because it is the media relay; where it cannot - no engine,
  // no call, or an engine that will not take the description - the message travels on
  // exactly as it arrived, which is the proxy behaviour the RFC describes.
  //
  // `then` runs when the message is ready to go: inline when there was nothing to do,
  // and on the strand from the engine's handler when there was.
  void _anchor_media(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<SIPMessage>& message, const std::shared_ptr<Channel>& outgoing,
                     const std::shared_ptr<Context>& context, std::function<void()> then);

  // RFC 3261 16.7 step 6: what goes back when every branch has been tried.
  void _send_best(const std::shared_ptr<Context>& context);
  void _rewrite_contact(const std::shared_ptr<SIPMessage>& message, const std::shared_ptr<Channel>& from) const;
  void _read_caller_profile(const std::shared_ptr<Context>& context, std::function<void()> then);
  bool _reoffer(const std::shared_ptr<Context>& context);
  void _add_targets(const std::shared_ptr<Context>& context, std::vector<types::Location> bindings) const;
  bool _try_other_flow(const std::shared_ptr<Context>& context);
  void _report_reoffer(const std::shared_ptr<Context>& context, bool took);

  // RFC 3261 16.10: the CANCEL for a branch already forwarded.
  void _cancel_branch(const std::shared_ptr<Context>& context);

  // RFC 3261 16.6 step 11, 16.7 step 2 and 16.8: timer C is what gives up on an INVITE
  // branch that goes on answering provisionally and never finishes. The transaction
  // layer will not: a provisional response moves the INVITE client transaction to
  // Proceeding and cancels timer B (17.1.1.2), so from the first 100 Trying onwards the
  // branch has no bound of its own at all.
  void _timer_c_start(const std::shared_ptr<Context>& context);
  void _timer_c_cancel(const std::shared_ptr<Context>& context);
  void _on_timer_c(const std::shared_ptr<Context>& context);

  // RFC 3261 16.10: a CANCEL this node has no response context for.
  void _forward_cancel_statelessly(const std::shared_ptr<SIPMessage>& cancel, const std::shared_ptr<transactions::TransactionBase>& transaction);

  void _send_status(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request, std::uint16_t code,
                    const std::string& reason);

  // Who may send this where (RFC 3261 22.3, and the 2026-10-01 decision). `then` runs only
  // for a request that may go on; anything else has been answered.
  void _authorize(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction, std::function<void()> then);
  void _authenticate(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction,
                     const std::shared_ptr<types::Realm>& realm, const std::shared_ptr<SIPUri>& caller, std::function<void()> then);
  void _send_proxy_challenge(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request,
                             const std::shared_ptr<types::Realm>& realm);

  bool _names_this_node(const SIPUri& uri) const;
  std::shared_ptr<Channel> _flow_to(const SIPUri& uri) const;

  // RFC 5626: the flow a binding was registered over is the route back to it, and for a
  // browser or a NAT'd client it is the only one - their Contact resolves to nothing
  // reachable. A UDP flow this node has forgotten is still sent down; a reliable one that
  // has closed leaves the Contact as all there is to go on.
  Target _target_for(const types::Location& binding) const;
  static std::shared_ptr<SIPUri> _datagram_hop(const std::string& flow_id);

  static NextHop _next_hop_of(const SIPUri& uri);

  // The realm's say over the transport's. Only FromTransport defers to the flow.
  static media::Flags::Profile _profile_under(const types::MediaPolicy& policy, const std::string& transport);

  std::shared_ptr<loggers::Logger> _logger;
  std::weak_ptr<Core> _core;

  // Response contexts by the id of the server transaction they answer, so a CANCEL can
  // find the branches its INVITE is still waiting on. Weak: the context belongs to the
  // callbacks in flight and must not outlive them.
  std::unordered_map<std::string, std::weak_ptr<Context>> _contexts;
};

}  // namespace athenasip
