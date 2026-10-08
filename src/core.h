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
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <ctime>
#include <future>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "address_discovery.h"
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
#include "node_directory.h"
#include "plugins/plugin.h"
#include "policy/policy.h"
#include "push/push_service.h"
#include "push_refresher.h"
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
class LocalUA;
class Registrar;

// The composition root. Core owns one strand, the registries (channels, transactions,
// dialogs, calls) and the transaction users, and routes a message to its transaction and a
// transaction to its TU. SIP semantics live in Registrar (RFC 3261 section 10) and Proxy
// (section 16).
//
// Every registry is touched only on the strand, so none is locked. Servers run their own
// io_context threads and reach Core through post() or call_on_strand().
class Core : public std::enable_shared_from_this<Core> {
 public:
  using Strand = boost::asio::strand<boost::asio::io_context::executor_type>;

  Core(std::shared_ptr<Logger> logger, std::shared_ptr<Config> _config, std::shared_ptr<Datastore> datastore, std::shared_ptr<events::EventSystem> events);

  const Strand& strand() const { return _strand; }

  // Queue work on the strand and return immediately.
  template <typename Fn>
  void post(Fn&& fn) {
    boost::asio::post(_strand, std::forward<Fn>(fn));
  }

  // Run work on the strand and wait for its result. Called on the strand it runs inline,
  // so it cannot deadlock on itself.
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

  // Realms, subscribers and nonces live in the datastore. These calls are async and never
  // block; each handler runs on the strand.
  void realm_get_by_name(std::string realm, plugins::Handler<std::shared_ptr<Realm>> handler);

  // Subscribers
  void subscriber_get(std::shared_ptr<SIPIdentity> identity, plugins::Handler<std::shared_ptr<Subscriber>> handler);
  // instance and reg_id identify an RFC 5626 outbound binding; empty and zero otherwise. push marks a binding
  // this node agreed to push to (RFC 8599).
  void subscriber_register(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel,
                           std::uint32_t expires_seconds, std::string path, plugins::StatusHandler handler, std::string instance = "", std::uint32_t reg_id = 0,
                           bool push = false);
  void subscriber_unregister(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel,
                             plugins::StatusHandler handler);
  void location_list(std::uint64_t subscriber_id, plugins::Handler<std::vector<types::Location>> handler);

  // Channels

  // A flow's name: "transport://host:port", transport lowercased. It is the registry key
  // and the flow id a binding records (RFC 5626). Always build it here.
  static std::string channel_key(const std::string& transport, const std::string& host, std::uint16_t port);
  static std::string channel_key(const std::string& transport, const std::string& endpoint);

  bool channel_register(std::string endpoint, std::shared_ptr<Channel> channel);
  bool channel_unregister(std::string endpoint, std::shared_ptr<Channel> channel);
  void channel_close_all();

  // The live flow to a next hop, or null (RFC 3261 16.6 step 7).
  std::shared_ptr<Channel> channel_find(const std::string& transport, const std::string& host, std::uint16_t port);

  // The same lookup by channel_key, which is what a binding records.
  std::shared_ptr<Channel> channel_find(const std::string& flow_id);

  // The flow named by the token in a Record-Route this node wrote, or null. It is how an
  // in-dialog request reaches an endpoint whose Contact is unreachable, such as a browser.
  std::shared_ptr<Channel> channel_for_token(const std::string& token);

  // Seals and opens channel flow tokens. See FlowTokens.
  const FlowTokens& flow_tokens() const { return _flow_tokens; }

  // The subscribers this node has had to re-offer in the other media profile.
  media::Reoffers& reoffers() { return _reoffers; }

  // The RFC 3263 server locator. Built on first use from /etc/resolv.conf unless one is set.
  std::shared_ptr<dns::SipLocator> locator();
  void locator_set(std::shared_ptr<dns::SipLocator> locator) { _locator = std::move(locator); }

  // The cluster's certificates, for the TLS flows this node opens to its peers. False when
  // they cannot be loaded.
  bool cluster_tls_set(const std::string& ca, const std::string& cert, const std::string& key);

  // The flow to a next hop, opening one when there is none (RFC 3261 18.1.1): TCP is
  // dialled, UDP leaves by a listener's socket, and TLS needs cluster_tls_set and reaches
  // only cluster peers. Bounded by Config::sip_connect_timeout_ms. The handler runs on the
  // strand.
  // trunk_ca: TLS to a trunk, verified against this CA file or, empty, the system's store, with no client
  // certificate. Without it, outbound TLS is to cluster peers only, with the cluster's certificates.
  void channel_connect(std::string transport, std::string host, std::uint16_t port, plugins::Handler<std::shared_ptr<Channel>> handler,
                       std::optional<std::string> trunk_ca = std::nullopt);

  // A second registry name for a channel: the name it was dialled by, so a hop named by
  // hostname reuses its connection.
  void channel_alias(std::string endpoint, const std::shared_ptr<Channel>& channel);

  // The "host:port" addresses that name this node in a Route or Request-URI (RFC 3261
  // 16.4). Channels add theirs as they register.
  void local_address_add(std::string host_port);
  bool is_local_address(const std::string& host, std::uint16_t port) const;

  // sip.public_address when set, otherwise the given local address.
  std::string advertised_address(const std::string& local_address) const {
    const auto public_address = config->public_address();
    return public_address.empty() ? local_address : public_address;
  }

  // The address this node writes in Via, Record-Route, Service-Route and its own requests
  // on a flow. A cluster peer gets the inter-node listener; a far end outside sip.localnet
  // gets the public address and forwarded port, when one is configured; anyone else gets
  // the local address and port. A wildcard listener gives the address it would send from.
  struct Advertised {
    std::string host;
    std::uint16_t port = 0;
  };
  Advertised advertised_for(const Channel& channel) const;

  // Nonce
  void nonce_create(std::shared_ptr<Realm> realm, plugins::Handler<std::string> handler);
  void nonce_check(std::string nonce, plugins::Handler<bool> handler);

  // Messages
  void process_message(std::shared_ptr<SIPMessage> message);

  // Transactions, keyed by the RFC 3261 17.1.3 / 17.2.3 identity from TransactionMatcher.
  void transaction_add(const std::string& key, std::shared_ptr<transactions::TransactionBase> transaction);
  std::shared_ptr<transactions::TransactionBase> transaction_get(const std::string& key);
  bool transaction_remove(const std::string& key);
  void transaction_end_all();
  std::size_t transaction_count() const;

  // Sends a request through a new client transaction, filed under the branch of the top
  // Via, which the caller has already added (RFC 3261 16.6 step 8).
  std::shared_ptr<transactions::TransactionBase> client_transaction_start(std::shared_ptr<SIPMessage> request, std::shared_ptr<Channel> channel,
                                                                          transactions::TransactionBase::TuFn to_tu,
                                                                          transactions::TransactionBase::TimeoutFn on_timeout);

  // Tests inject a ManualTimerSource here.
  void timer_source_set(std::shared_ptr<TimerSource> source) { _timer_source = std::move(source); }

  // The timer source's clock. Compare times against this, never steady_clock directly, so
  // tests can advance it.
  std::chrono::steady_clock::time_point now() const { return _timer_source->now(); }

  // For transaction users with timers of their own, such as the proxy's timer C (RFC 3261
  // 16.6 step 11). Read it at schedule time; do not hold it.
  std::shared_ptr<TimerSource> timer_source() const { return _timer_source; }

  // Dialogs (RFC 3261 section 12), tracked but not owned: the node watches the dialogs it
  // record-routed to know when a call ends. Nothing routes on them.
  std::shared_ptr<Dialogs> dialogs();

  // OPTIONS to registered clients, where their realm asks for it. See Qualifier.
  std::shared_ptr<Qualifier> qualifier();

  // Requests this node sends as a user agent of its own. See LocalUA.
  std::shared_ptr<LocalUA> local_ua();

  // A request this node originates, routed by the proxy as if it had arrived. Its answer comes back to on_final
  // rather than going anywhere. On the strand.
  void local_request(std::shared_ptr<SIPMessage> request, transactions::TransactionBase::SendFn on_final);

  // Whether a URI, as a Route or Record-Route, names this node.
  bool names_this_node(const SIPUri& uri) const;

  // Calls
  bool call_register(std::shared_ptr<Call> call);
  bool call_unregister(std::string callId);
  std::shared_ptr<Call> call_get(std::string callId);

  // A snapshot of live calls. On the strand.
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

  // Routing and authorisation policy (policy.url). builtin:// until one is registered, so a Core built without
  // one, as the tests build it, behaves as a node with no script.
  void policy_register(std::shared_ptr<policy::Policy> policy);
  std::shared_ptr<policy::Policy> policy();

  // Has the policy read its rules again (SIGHUP, POST /api/v1/policy/reload). On the strand. Empty on success, else
  // why not; the rules in force stay until a reload succeeds. A success is reported in the node's status at once.
  std::string policy_reload();

  // RFC 8599 push notification services, filed by pn-provider. None unless push.urls names some.
  void push_register(std::shared_ptr<push::PushService> service);
  std::shared_ptr<push::PushService> push_service(const std::string& provider) const;
  const std::map<std::string, std::shared_ptr<push::PushService>>& push_services() const { return _push_services; }

  // This node's own address as STUN servers see it (AddressDiscovery), and the answers that arrive for it.
  std::shared_ptr<AddressDiscovery> address_discovery();
  void stun_answered(const stun::Mapped& mapped);

  // Refresh pushes for push bindings (RFC 8599 5.5).
  std::shared_ptr<PushRefresher> push_refresher();

  // RFC 3261 10.3 step 1: a REGISTER for a domain this node does not serve, for the proxy to forward.
  void register_forward(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction);

  // The registrar has stored a binding; a request held for that client's push goes now (RFC 8599 5.6.2).
  void binding_registered(std::uint64_t subscriber_id, const std::shared_ptr<SIPUri>& contact);

  std::shared_ptr<Config> config;
  std::shared_ptr<datastores::Datastore> datastore;
  std::shared_ptr<events::EventSystem> events;

  // The version this node reports, set by main from the build.
  void version_set(std::string version) { _version = std::move(version); }

  // Starts expiring idle UDP flows (sip.flow_idle_timeout). Call after construction: it
  // needs weak_from_this.
  void flow_sweep_start();

  // Starts publishing this node's status on events.status_interval. Call once, after the
  // datastore and the bus are connected, or the first report says degraded.
  void node_status_start();

  // The node's status report as JSON, the same one the HTTP health endpoint gives.
  std::string node_status_json(const std::string& status) const;

  // The same report built from parts. Static because the bus's last-will message has to
  // be built before the bus connects, which is before a Core exists.
  static std::string node_status_json(const std::string& status, const std::string& node_id, const std::string& version, const std::string& datastore,
                                      std::int64_t uptime, std::uint32_t status_interval, const std::vector<Config::AdvertisedTransport>& transports = {},
                                      const std::optional<Config::AdvertisedTransport>& cluster = std::nullopt);

  // Every node's status as heard on the bus, this one's included.
  std::shared_ptr<NodeDirectory> nodes() const { return _nodes; }

  // Publishes "stopped" on a clean shutdown, replacing the retained status.
  void node_status_stop();
  std::shared_ptr<media::MediaEngine> media;

 private:
  // Builds the transaction users on first use: they need shared_from_this.
  void _ensure_transaction_users();

  // Creates, files and starts a server transaction, which delivers the request to its TU.
  std::shared_ptr<transactions::TransactionBase> _server_transaction_start(const std::shared_ptr<SIPMessage>& request);

  void _deliver_to_tu(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction);

  void _send_status(const std::shared_ptr<SIPMessage>& request, uint16_t code, const std::string& reason);

  std::shared_ptr<loggers::Logger> _logger;

  Strand _strand;

  // Everything below is strand-confined.
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
  std::shared_ptr<LocalUA> _local_ua;
  std::shared_ptr<PushRefresher> _push_refresher;
  std::shared_ptr<AddressDiscovery> _address_discovery;
  std::map<std::string, std::shared_ptr<push::PushService>> _push_services;
  std::shared_ptr<policy::Policy> _policy;
  std::shared_ptr<dns::SipLocator> _locator;
  std::shared_ptr<Dialogs> _dialogs;

  // Keeps the Call record in step with the dialog it is a leg of.
  void _on_dialog_change(const std::shared_ptr<types::Dialog>& dialog);

  std::unordered_map<std::string, std::shared_ptr<Call>> _calls;

  // The call sweep ends calls whose far end vanished without a BYE: by media silence
  // (sip.media_timeout, anchored calls only) and by age (sip.max_call_duration, every call).
  std::shared_ptr<Timer> _call_sweep_timer;
  std::shared_ptr<Timer> _flow_sweep_timer;

  std::shared_ptr<Timer> _node_status_timer;
  std::shared_ptr<NodeDirectory> _nodes = std::make_shared<NodeDirectory>();
  std::string _version;
  std::time_t _started_at = 0;

  void _node_status_schedule();
  void _node_status_publish();

  void _call_sweep_schedule();
  void _call_sweep();

  // Closes the records of calls whose node has gone, or that this node held before it restarted. Run by the live
  // node with the lowest id; an anchored call is closed only once its engine says its media has stopped.
  void _orphan_sweep();
  void _settle_orphan(const std::shared_ptr<Call>& call);
  void _close_orphan(const std::shared_ptr<Call>& call, const std::string& reason, bool release);

  // When the sweep began, by the timer source: a node judges another gone only after three status intervals.
  std::optional<std::chrono::steady_clock::time_point> _watching_since;
  std::time_t _constructed_at = std::time(nullptr);

  // channel_connect's TLS half: the client side of the cluster's mutual TLS.
  void _secure_flow(std::shared_ptr<boost::asio::ip::tcp::socket> socket, std::shared_ptr<boost::asio::ssl::context> context, const std::string& host,
                    const std::string& key, std::function<void(plugins::Result<std::shared_ptr<Channel>>)> answer);
  std::shared_ptr<boost::asio::ssl::context> _cluster_tls;

  // Client contexts for TLS to trunks, by CA file ("" for the system's store), built on first use.
  std::map<std::string, std::shared_ptr<boost::asio::ssl::context>> _trunk_tls;
  std::shared_ptr<boost::asio::ssl::context> _trunk_tls_for(const std::string& ca);

  // channel_connect's UDP half.
  void _connect_datagram(std::string host, std::uint16_t port, plugins::Handler<std::shared_ptr<Channel>> handler);
  void _open_datagram(std::vector<boost::asio::ip::udp::endpoint> candidates, std::string key, plugins::Handler<std::shared_ptr<Channel>> handler);

  // Closes UDP flows idle for sip.flow_idle_timeout.
  void _flow_sweep_schedule();
  void _flow_sweep();

  // Ends a call this node's own policy has decided is over (media silence, the duration
  // cap), with a BYE to each end. An RFC 4028 lapse is the Dialogs sweep's and sends none.
  void _end_held_call(const std::string& call_id, const std::string& reason);
};

}  // namespace athenasip