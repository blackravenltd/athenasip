//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <future>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "api/admin_api.h"
#include "call.h"
#include "config.h"
#include "datastores/datastore.h"
#include "dialogs.h"
#include "dns/sip_locator.h"
#include "events/event_system.h"
#include "expiry_set.h"
#include "flow_tokens.h"
#include "global_io_context.h"
#include "loggers/logger.h"
#include "loggers/logger_scoped.h"
#include "media/media_engine.h"
#include "media/reoffers.h"
#include "plugins/plugin.h"
#include "rtp/rtp_relay_set.h"
#include "servers/server.h"
#include "sip_message.h"
#include "timer_source.h"
#include "transactions/transaction_base.h"
#include "transactions/transaction_matcher.h"

using namespace athenasip::types;
using namespace athenasip::datastores;
using namespace athenasip::events;
using namespace athenasip::servers;
using namespace athenasip::rtp;
using namespace athenasip::api;

namespace athenasip {

class Proxy;
class Qualifier;
class Registrar;

// Core runs on a single strand. Every registry it owns - channels, transactions, dialogs
// and calls - is touched only from that strand, so none of them needs a lock. Servers each run their own io_context on their own thread and post
// into the strand rather than reaching into Core directly.
//
// Methods below are marked "on the strand" where they touch that state. Call them from
// strand work: either from inside other strand work, or through post() and
// call_on_strand() from another thread.
//
// Core is the composition root: it owns the strand, the registries and the transaction
// users, and routes a message to its transaction and a transaction to its TU. It holds
// no SIP semantics of its own - those live in Registrar (section 10) and Proxy
// (section 16).
class Core : public std::enable_shared_from_this<Core> {
 public:
  using Strand = boost::asio::strand<boost::asio::io_context::executor_type>;

  Core(std::shared_ptr<Logger> logger, std::shared_ptr<Config> _config, std::shared_ptr<Datastore> datastore, std::shared_ptr<events::EventSystem> events);

  // The serialisation domain for everything Core owns.
  const Strand& strand() const { return _strand; }

  // Queue work on the strand and return immediately.
  template <typename Fn>
  void post(Fn&& fn) {
    boost::asio::post(_strand, std::forward<Fn>(fn));
  }

  // Run work on the strand and wait for its result. For callers that are not on the
  // strand and need an answer: shutdown, the admin API, and tests. Running already on
  // the strand it calls straight through, so this cannot deadlock on itself.
  template <typename Fn>
  auto call_on_strand(Fn&& fn) -> decltype(fn()) {
    if (_strand.running_in_this_thread()) return fn();

    std::packaged_task<decltype(fn())()> task(std::forward<Fn>(fn));
    auto result = task.get_future();

    boost::asio::post(_strand, [task = std::move(task)]() mutable { task(); });

    return result.get();
  }

  // Servers
  void server_register(std::shared_ptr<Server> server);
  void server_start_all();
  void server_stop_all();

  // Realms, accounts and nonces all live in the datastore, which is async by
  // contract, so these are too: the handler runs back on the strand once the datastore
  // answers. Nothing here blocks, because blocking here would stop every call on the
  // node rather than only the one that asked.
  void realm_get_by_name(std::string realm, plugins::Handler<std::shared_ptr<Realm>> handler);

  // Accounts
  void account_get(std::shared_ptr<SIPIdentity> identity, plugins::Handler<std::shared_ptr<Account>> handler);
  void account_register(std::shared_ptr<Account> account, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel, std::uint32_t expires_seconds,
                        std::string path, plugins::StatusHandler handler);
  void account_unregister(std::shared_ptr<Account> account, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel, plugins::StatusHandler handler);
  void location_list(std::uint64_t account_id, plugins::Handler<std::vector<types::Location>> handler);

  // Channels

  // The one name a flow has: "transport://host:port", lowercased. The registry files a
  // channel under it, channel_find answers to it, and a binding records it as the flow
  // it was learned over (RFC 5626). Built here rather than at each call site, because a
  // key built two ways is a lookup that silently misses.
  static std::string channel_key(const std::string& transport, const std::string& host, std::uint16_t port);
  static std::string channel_key(const std::string& transport, const std::string& endpoint);

  bool channel_register(std::string endpoint, std::shared_ptr<Channel> channel);
  bool channel_unregister(std::string endpoint, std::shared_ptr<Channel> channel);
  void channel_close_all();

  // The live flow to a next hop, or null when this node has none (RFC 3261 16.6 step 7).
  // Channels are filed under channel_key, which is what a next hop resolves to once its
  // URI has given up its transport, host and port.
  std::shared_ptr<Channel> channel_find(const std::string& transport, const std::string& host, std::uint16_t port);

  // The same lookup by the name itself, which is what a binding recorded (RFC 5626).
  std::shared_ptr<Channel> channel_find(const std::string& flow_id);

  // The flow an opaque token names, or null when it names none. The token is what this
  // node put in the Record-Route it wrote, and it is the only way an in-dialog request
  // reaches an endpoint whose Contact resolves to nothing - a browser's always does.
  std::shared_ptr<Channel> channel_for_token(const std::string& token);

  // What every channel's token is sealed with, and what opens one whose channel this node
  // no longer holds. See FlowTokens.
  const FlowTokens& flow_tokens() const { return _flow_tokens; }

  // The accounts this node has had to offer the other media profile, for the operator.
  media::Reoffers& reoffers() { return _reoffers; }

  // RFC 3263: where a request for a SIP URI naming a host goes. Made on first use from the
  // system's nameservers (/etc/resolv.conf); a test, or a composition root that knows
  // better, sets its own.
  std::shared_ptr<dns::SipLocator> locator();
  void locator_set(std::shared_ptr<dns::SipLocator> locator) { _locator = std::move(locator); }

  // The flow to a next hop, opening one when this node has none.
  //
  // A registered client is reached on the connection this node accepted, which
  // channel_find already answers. A trunk, a peer node, or a request too large to leave
  // over UDP (18.1.1) has nothing listening back this way and has to be dialled. Until
  // this existed such a hop was answered 480, because nothing in the tree opened a
  // connection rather than accepting one.
  //
  // Async because it is a DNS lookup and a TCP handshake, and bounded by
  // Config::sip_connect_timeout_ms because the operating system's own bound is far
  // longer than the transaction has. The handler runs on the strand.
  //
  // UDP is opened through a listener's socket rather than dialled, and only TLS is refused.
  void channel_connect(std::string transport, std::string host, std::uint16_t port, plugins::Handler<std::shared_ptr<Channel>> handler);

  // A second name for a channel already registered: the name it was dialled by, when
  // that is not the address it resolved to. Without it every request to a hop named by
  // hostname would resolve and dial again, and the node would hold one connection per
  // request.
  void channel_alias(std::string endpoint, const std::shared_ptr<Channel>& channel);

  // The addresses this node answers on, as "host:port". A Route or a Request-URI naming
  // one of these names this node (RFC 3261 16.4), which is how a Record-Route this node
  // wrote is recognised when it comes back. Channels add their local endpoint as they
  // register, so the set is what the node is actually reachable at rather than what it
  // was configured with.
  void local_address_add(std::string host_port);
  bool is_local_address(const std::string& host, std::uint16_t port) const;

  // The address this node writes into a Via, a Record-Route or a Service-Route: the one
  // it was told to advertise, or the address the flow is actually on when it was told
  // nothing. A UDP listener is bound to the wildcard, so its local endpoint is 0.0.0.0,
  // and anything pointing the far end at 0.0.0.0 is something it cannot come back to.
  std::string advertised_address(const std::string& local_address) const {
    return config->sip_public_address.empty() ? local_address : config->sip_public_address;
  }

  // Nonce
  void nonce_create(std::shared_ptr<Realm> realm, plugins::Handler<std::string> handler);
  void nonce_check(std::string nonce, plugins::Handler<bool> handler);

  // Messages
  void process_message(std::shared_ptr<SIPMessage> message);

  // Transactions. The key is the RFC 3261 17.1.3 / 17.2.3 identity, which
  // TransactionMatcher computes; nothing outside this file invents one.
  void transaction_add(const std::string& key, std::shared_ptr<transactions::TransactionBase> transaction);
  std::shared_ptr<transactions::TransactionBase> transaction_get(const std::string& key);
  bool transaction_remove(const std::string& key);
  void transaction_end_all();
  std::size_t transaction_count() const;

  // Sends a request through a new client transaction: the machine is chosen from the
  // method, filed under the request's own branch, and started. The caller has already
  // put its Via on top (RFC 3261 16.6 step 8), which is what the response will match on.
  std::shared_ptr<transactions::TransactionBase> client_transaction_start(std::shared_ptr<SIPMessage> request, std::shared_ptr<Channel> channel,
                                                                          transactions::TransactionBase::TuFn to_tu,
                                                                          transactions::TransactionBase::TimeoutFn on_timeout);

  // Tests drive the section 17 timers from a ManualTimerSource rather than a real clock.
  void timer_source_set(std::shared_ptr<TimerSource> source) { _timer_source = std::move(source); }

  // The node's sense of time, which is the timer source's. Anything comparing two moments
  // has to read it here rather than from steady_clock, or the two are the same in
  // production and drift apart the moment a test advances one of them.
  std::chrono::steady_clock::time_point now() const { return _timer_source->now(); }

  // For the transaction users that keep timers of their own: timer C is the proxy's
  // (RFC 3261 16.6 step 11), not the transaction layer's. Read at schedule time rather
  // than held, so a source a test swaps in afterwards is the one that gets used.
  std::shared_ptr<TimerSource> timer_source() const { return _timer_source; }

  // Dialogs (RFC 3261 section 12). Tracked, not owned: the node is on the path of every
  // request in a dialog because it record-routed, and it watches them go by so that it
  // knows when a call has ended. Nothing routes on this.
  std::shared_ptr<Dialogs> dialogs();

  // OPTIONS to registered clients, where their realm asks for it. See Qualifier.
  std::shared_ptr<Qualifier> qualifier();

  // Calls
  bool call_register(std::shared_ptr<Call> call);
  bool call_unregister(std::string callId);
  std::shared_ptr<Call> call_get(std::string callId);

  // On the strand, for the admin API and /metrics: what is live, by snapshot.
  std::vector<std::shared_ptr<Call>> call_list() const;
  std::size_t call_count() const { return _calls.size(); }

  // Open flows by transport, each counted once however many names it is filed under.
  std::map<std::string, std::size_t> channel_counts() const;

  // Admin API
  void admin_register(std::shared_ptr<api::AdminAPI> adminAPI);
  void admin_start();
  void admin_stop();

  // Media engine
  void media_register(std::shared_ptr<media::MediaEngine> engine);

  std::shared_ptr<Config> config;
  std::shared_ptr<datastores::Datastore> datastore;
  std::shared_ptr<events::EventSystem> events;

  // What this node reports itself as. Set by main from the build version, because Core
  // is the composition root and the thing that announces the node, but the version is
  // the binary's fact rather than the composition's.
  void version_set(std::string version) { _version = std::move(version); }

  // Start saying, on an interval, that this node is alive and what it is.
  //
  // Called once, after the datastore and the bus are up, because the first thing it
  // publishes is a health report and a report written before the datastore connected
  // would say degraded about a node that is fine.
  // Starts forgetting connectionless flows that have gone quiet. Separate from the
  // constructor because it schedules against weak_from_this, which a constructor has not
  // got yet.
  void flow_sweep_start();

  void node_status_start();

  // The same report the HTTP health endpoint gives, as JSON. A monitor reading one and
  // a monitor reading the other should not disagree about the node.
  std::string node_status_json(const std::string& status) const;

  // The same report, built from parts rather than from this node's state.
  //
  // Static because the will has to be built before a Core exists: a broker takes a will
  // when the session opens, and the bus is connected before the composition root that
  // would otherwise own this is built. Setting it afterwards is silently too late,
  // which is exactly the bug this shape exists to make impossible.
  static std::string node_status_json(const std::string& status, const std::string& node_id, const std::string& version, const std::string& datastore,
                                      std::int64_t uptime);

  // The last thing a node says on the way out, so the retained message does not claim
  // for ever that a node which stopped cleanly is still up.
  void node_status_stop();
  std::shared_ptr<media::MediaEngine> media;

 private:
  // The transaction users are built on first use: they hold a Core and shared_from_this
  // is not available while the constructor runs.
  void _ensure_transaction_users();

  // Creates, files and starts the server transaction for a request. Starting it is what
  // delivers the request to its TU.
  std::shared_ptr<transactions::TransactionBase> _server_transaction_start(const std::shared_ptr<SIPMessage>& request);

  // Routes a request to the transaction user that owns its method.
  void _deliver_to_tu(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction);

  void _send_status(const std::shared_ptr<SIPMessage>& request, uint16_t code, const std::string& reason);

  std::shared_ptr<loggers::Logger> _logger;

  Strand _strand;

  // All of the following are strand-confined. No locks.
  std::unordered_map<std::string, std::shared_ptr<Channel>> _channels;
  std::unordered_map<std::string, std::shared_ptr<Channel>> _channels_by_token;
  std::set<std::string> _local_addresses;

  FlowTokens _flow_tokens;
  media::Reoffers _reoffers;

  std::vector<std::shared_ptr<Server>> _servers;

  std::shared_ptr<api::AdminAPI> _adminAPI;

  std::shared_ptr<ExpirySet<std::string>> _nonce_cache;

  transactions::TransactionMatcher _matcher;
  std::shared_ptr<TimerSource> _timer_source = default_timer_source();

  std::shared_ptr<Registrar> _registrar;
  std::shared_ptr<Proxy> _proxy;
  std::shared_ptr<Qualifier> _qualifier;
  std::shared_ptr<dns::SipLocator> _locator;
  std::shared_ptr<Dialogs> _dialogs;

  // Keeps the Call record in step with the dialog it is a leg of.
  void _on_dialog_change(const std::shared_ptr<types::Dialog>& dialog);

  std::unordered_map<std::string, std::shared_ptr<Call>> _calls;

  // The sweep that decides a call this node is holding is over. A call between endpoints
  // that never negotiated a session timer has no expiry of its own, and a phone that
  // loses power sends no BYE, so without this the node holds that call's dialog, record
  // and relay ports until it restarts.
  //
  // Two questions, because neither covers everything. How long the media has been silent
  // reaches any call this node anchors and says nothing about one whose media went end to
  // end; how long the call has been up reaches every call and cannot tell a live one from
  // a dead one.
  std::shared_ptr<Timer> _call_sweep_timer;
  std::shared_ptr<Timer> _flow_sweep_timer;

  std::shared_ptr<Timer> _node_status_timer;
  std::string _version;
  std::time_t _started_at = 0;

  void _node_status_schedule();
  void _node_status_publish();

  void _call_sweep_schedule();
  void _call_sweep();

  // Forgetting a connectionless flow that has gone quiet. See sip.flow_idle_timeout.
  // channel_connect's UDP half. See there.
  void _connect_datagram(std::string host, std::uint16_t port, plugins::Handler<std::shared_ptr<Channel>> handler);
  void _open_datagram(std::vector<boost::asio::ip::udp::endpoint> candidates, std::string key, plugins::Handler<std::shared_ptr<Channel>> handler);

  void _flow_sweep_schedule();
  void _flow_sweep();

  // RFC 4028 section 8.3, which is the only thing a proxy may do about a call it has
  // decided is over: "the proxy MAY remove associated call state, and MAY free any
  // resources associated with the call. Unlike the UA, it MUST NOT send a BYE." This node
  // is on the path of the dialog, not an end of it.
  void _end_held_call(const std::string& call_id, const std::string& reason);
};

}  // namespace athenasip