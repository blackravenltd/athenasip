//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/network_v4.hpp>
#include <boost/asio/ip/network_v6.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "loggers/logger.h"
#include "loggers/logger_scoped.h"
#include "types/realm.h"
#include "types/url.h"

using namespace athenasip::loggers;

namespace athenasip {

class Config {
 public:
  Config(std::shared_ptr<Logger> logger);

  // Field names follow the YAML keys: sip_node_id is sip.node_id.
  std::string sip_node_id;

  // Permits SIP over plain UDP, TCP and WS.
  bool sip_allow_unencrypted = true;

  // Log every message in full, not only its first line. Credentials are redacted.
  bool sip_log_messages = false;

  // sip.forward_register: RFC 3261 10.3 step 1. "subscribers" forwards a REGISTER for a domain this node does
  // not serve when one of this node's subscribers sends it; "never" answers 403.
  std::string sip_forward_register = "subscribers";

  // The address this node tells others to reach it on. Empty falls back to the bind
  // address, which is no use to a client on a wildcard bind.
  std::string sip_public_address;

  // sip.public_address, or else an address peers have verified this node at (AddressDiscovery), or empty. Read
  // it rather than sip_public_address wherever the node says where it is. Safe from any thread.
  std::string public_address() const;
  void discovered_address_set(std::string address);

  // sip.localnet: prefixes on this node's side of the router. A far end inside them is
  // given the local address and port; everyone else the public ones. parse_localnet
  // returns false when an entry is not a prefix or an address.
  std::vector<std::string> sip_localnet;
  bool parse_localnet();
  bool in_localnet(const boost::asio::ip::address& address) const;

  // The public port for udp, tcp, tls, ws or wss, or zero.
  std::uint16_t public_port_for(const std::string& transport) const;

  // Where this node can be reached, one entry per enabled transport: at sip.public_address
  // when set, otherwise the bind address.
  struct AdvertisedTransport {
    std::string transport;
    std::string address;
    std::uint16_t port = 0;
    bool secure = false;

    std::string uri() const;
  };
  std::vector<AdvertisedTransport> advertised_transports() const;

  // Where a peer reaches the inter-node listener, or nullopt outside a cluster:
  // cluster.advertise, else the bind address, else sip.public_address on a wildcard bind.
  std::optional<AdvertisedTransport> advertised_cluster() const;

  // RFC 4028 section 8.1: the session interval, in seconds, put on a call that asked for
  // none. Zero inserts none. Minimum 90; the RFC recommends 1800.
  uint32_t sip_session_expires = 1800;

  // RFC 4028 section 8.1: add Require: timer when the caller did not advertise support.
  // NOT RECOMMENDED by the RFC: an endpoint without the extension answers 420 and the call
  // fails. Only for a closed fleet of known handsets.
  bool sip_require_session_timer = false;

  // RFC 4028 section 5: the shortest session interval a call may negotiate, in seconds.
  // Minimum 90.
  uint32_t sip_session_min_se = 90;

  // sip.timers: the RFC 3261 section 17 timers.
  uint16_t sip_timer_t1_rtt_ms = 500;
  uint16_t sip_timer_t2_max_retransmit_interval_ms = 4000;
  uint16_t sip_timer_t4_network_propagation_ms = 5000;
  uint16_t sip_timer_a_invite_initial = 1;
  uint16_t sip_timer_b_invite_timeout = 64;
  uint16_t sip_timer_d_invite_duration = 64;
  uint16_t sip_timer_e_non_invite_initial = 1;
  uint16_t sip_timer_f_non_invite_timeout = 64;
  uint16_t sip_timer_g_server_invite_initial = 1;
  uint16_t sip_timer_h_server_invite_timeout = 64;
  uint16_t sip_timer_i_server_invite_duration = 1;
  uint16_t sip_timer_j_server_non_invite_duration = 64;
  uint16_t sip_timer_k_non_invite_duration = 1;

  // The longest any call is held, in seconds. Zero is no cap. It also cuts legitimate calls
  // of that length, so set it only where calls are known to be shorter.
  uint32_t sip_max_call_duration = 0;

  // How long an anchored call may carry no media before this node drops its state, in
  // seconds. Zero turns it off. It catches an endpoint that vanished without a BYE. RTCP
  // counts as media, so a call on hold is not silent (RFC 3550 section 6).
  uint32_t sip_media_timeout = 300;

  // RFC 3261 16.6 step 11, timer C: how long a proxied INVITE may stay in a provisional
  // state. Must be larger than three minutes.
  uint32_t sip_timer_c_invite_proxy_ms = 240000;

  // How long opening a flow to a next hop may take (RFC 3261 16.6 step 7). Short enough
  // that a fork can try a second target inside timer B's 32 seconds.
  uint32_t sip_connect_timeout_ms = 4000;

  // How long a UDP flow may sit idle before it is forgotten, in seconds. Zero never
  // forgets. Keep it above 32 seconds (64*T1), so no live transaction loses its flow.
  std::uint32_t sip_flow_idle_timeout = 300;

  // tls, tcp, udp and websocket: the listeners. public_port is the port a router forwards
  // to the listener; zero means the bound port.
  bool tls_enable = false;
  std::string tls_address;
  uint16_t tls_port = 0;
  uint16_t tls_public_port = 0;
  std::string tls_cert_pem_filename;
  std::string tls_key_pem_filename;

  bool tcp_enable = false;
  std::string tcp_address;
  uint16_t tcp_port = 0;
  uint16_t tcp_public_port = 0;

  bool udp_enable = false;
  std::string udp_address;
  uint16_t udp_port = 0;
  uint16_t udp_public_port = 0;

  bool websocket_enable = false;
  std::string websocket_address;
  uint16_t websocket_port = 0;
  uint16_t websocket_public_port = 0;

  // websocket.tls: serve wss (RFC 7118 over TLS) on websocket.port. Needs the websocket
  // section's own certificate and key.
  bool websocket_tls = false;

  // websocket.secure_port: a second, wss listener beside a plain one. Zero is none. Unused
  // when websocket.tls is set.
  std::uint16_t websocket_secure_port = 0;

  // The secure_port listener's certificate: the websocket section's, or the tls section's.
  std::string websocket_cert() const { return websocket_cert_pem_filename.empty() ? tls_cert_pem_filename : websocket_cert_pem_filename; }
  std::string websocket_key() const { return websocket_key_pem_filename.empty() ? tls_key_pem_filename : websocket_key_pem_filename; }
  std::string websocket_cert_pem_filename;
  std::string websocket_key_pem_filename;

  // datastore.url. The default needs no external service; a cluster uses redis://.
  std::string db_url = "memory://";  // or "redis://127.0.0.1:6379"

  // events.url. The default is in-process; a cluster names its broker.
  std::string events_url = "local://";  // or "mqtt://user:pass@127.0.0.1:1883/athenasip?client_id=sip-01&keep_alive=30"

  // Seconds between this node's status reports on the bus. Zero publishes once.
  std::uint32_t events_status_interval = 30;

  // media.url
  std::string media_url = "builtin://";  // or "rtpengine://host:port"

  // plugins.path: directories of plugin modules (.so, .dylib, .dll) loaded at start. None by default.
  std::vector<std::string> plugins_path;

  // push.urls: RFC 8599 push notification services, one driver each ("apns://", "fcm://", "webpush://"). None by default.
  std::vector<std::string> push_urls;

  // push.timeout: seconds a request waits for the client to re-register after a push (RFC 8599 5.6.2). Kept
  // well inside timer F, so a non-INVITE request is answered before its sender gives up.
  std::uint32_t push_timeout = 10;

  // push.refresh: seconds before a push binding expires that a push asks the client to refresh it (5.5). Over
  // 120, which is also what +sip.pnsreg carries.
  std::uint32_t push_refresh = 180;

  // The shortest binding that can carry push: one that would expire before its refresh push is not accepted.
  std::uint32_t push_minimum_expiry() const { return push_refresh + 60; }

  // http: the admin API and the file server.
  std::string http_address;
  uint16_t http_port = 0;
  bool http_api_enable = false;

  // behaviour: the server's defaults for what differs between SIP servers. A realm
  // overrides any of them (types::Behaviour).
  types::MediaPolicy behaviour;

  // behaviour.qualify_interval: seconds between OPTIONS to a registered client. Zero is never.
  std::uint32_t behaviour_qualify_interval = 0;

  // cluster: the inter-node listener, mutual TLS under the cluster CA
  // (docs/certificates.md). The same certificates secure the flows this node opens to peers.
  bool cluster_enable = false;
  std::string cluster_address = "0.0.0.0";
  std::uint16_t cluster_port = 5062;

  // cluster.advertise: what peers dial. It must be a name or address this node's
  // certificate carries. Empty derives it; see advertised_cluster.
  std::string cluster_advertise;
  std::string cluster_ca;
  std::string cluster_cert;
  std::string cluster_key;

  // behaviour.rewrite_contact, the server's default for types::Behaviour::rewrite_contact.
  bool behaviour_rewrite_contact = false;

  bool http_files_enable = false;
  std::string http_files_path;

  // http.files.spa: answer a path that matches no file with index.html, so client-side
  // routes survive a reload. Never for /api/ or a path with a file extension. False makes
  // it a plain file server.
  bool http_files_spa = true;

  // An admin session's absolute lifetime and idle timeout, in seconds. The lifetime cannot
  // be zero; a zero idle timeout turns that check off.
  std::uint32_t http_api_session_lifetime = 12 * 60 * 60;
  std::uint32_t http_api_session_idle = 60 * 60;

  // http.api.rate_limits: a burst allowance, then a rate per minute. Zero turns either
  // off, as for an API behind a proxy that already limits.
  struct RateLimit {
    std::uint32_t burst = 0;
    std::uint32_t per_minute = 0;
  };

  // Open routes, unknown endpoints and credentials that do not resolve, by source address.
  RateLimit http_api_limit_open{30, 30};
  // The login on top of that, by source address and by username.
  RateLimit http_api_limit_login_source{10, 5};
  RateLimit http_api_limit_login_user{5, 1};
  // A signed-in caller, by session.
  RateLimit http_api_limit_session{60, 300};

  // log.level (debug, info, warn, error) and log.format (text, json).
  loggers::LogLevel log_level = loggers::LogLevel::DEBUG;
  loggers::LogFormat log_format = loggers::LogFormat::Text;

  // calls.history_retention: how long an ended call's record is kept, in seconds. Zero
  // keeps records for ever.
  std::uint32_t calls_history_retention = 30 * 24 * 60 * 60;

  // http.tls: an HTTPS listener beside the plain one. Browsers give the microphone only to
  // a secure page, so the console's softphone needs it.
  bool http_tls_enable = false;
  std::string http_tls_address;
  std::uint16_t http_tls_port = 8443;
  std::string http_tls_cert_pem_filename;
  std::string http_tls_key_pem_filename;

  // The certificate and key the HTTPS listener uses: its own, or the tls section's.
  std::string http_tls_cert() const { return http_tls_cert_pem_filename.empty() ? tls_cert_pem_filename : http_tls_cert_pem_filename; }
  std::string http_tls_key() const { return http_tls_key_pem_filename.empty() ? tls_key_pem_filename : http_tls_key_pem_filename; }

  // http.api.ice_servers: the ICE servers a web client is told to use, served by
  // `GET /api/v1/subscriber/{realm}/config`. It does not affect this node's own media.
  struct IceServer {
    // stun:host:port or turn:host:port (RFC 7064, RFC 7065). Only turn: URLs get credentials.
    std::string url;
  };

  std::vector<IceServer> ice_servers;

  // The secret shared with the TURN server (coturn's `static-auth-secret`), from which
  // expiring credentials are minted. Empty serves turn: URLs without credentials.
  std::string turn_shared_secret;

  // How long a minted TURN credential lasts, in seconds. Cannot be zero.
  std::uint32_t turn_credential_ttl = 3600;

  bool load_from_yaml(const std::string& filename);

  // A plugin's own section: the one named after the driver's name(), inside the section
  // for its kind. For what the URL cannot express.
  //
  //   datastore:
  //     url: redis://127.0.0.1:6379
  //     redis:
  //       health_check_interval: 5
  YAML::Node plugin_root(const std::string& kind, const std::string& name) const;

  // The whole file as read.
  const YAML::Node& root() const { return _root; }

  // The configuration in effect, defaults included, as YAML: what `athenasip --print-config`
  // prints. Plugin sections are copied through from the file.
  std::string effective_yaml() const;

 private:
  std::shared_ptr<Logger> _logger;

  // The document as loaded, kept only to hand plugins their sections.
  YAML::Node _root;

  // Shared, so a copy of a Config sees the same discovery.
  struct Discovered {
    mutable std::mutex mutex;
    std::string address;
  };
  std::shared_ptr<Discovered> _discovered = std::make_shared<Discovered>();

  std::vector<boost::asio::ip::network_v4> _localnet_v4;
  std::vector<boost::asio::ip::network_v6> _localnet_v6;
};

}  // namespace athenasip
