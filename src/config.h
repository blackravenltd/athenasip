//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "loggers/logger.h"
#include "loggers/logger_scoped.h"
#include "types/url.h"

using namespace athenasip::loggers;

namespace athenasip {

class Config {
 public:
  Config(std::shared_ptr<Logger> logger);

  // SIP configuration
  std::string sip_node_id;
  uint32_t sip_registration_timeout = 5000;

  // Allow SIP over an unencrypted transport. WSS is required for browsers, and TLS is
  // what a cluster talks, so this is the switch that permits plain UDP, TCP and WS.
  bool sip_allow_unencrypted = true;

  std::string sip_event_prefix;

  // The address this node tells the outside world to reach it on. A node bound to
  // 0.0.0.0 knows every address it answers on and none that a client should use, so
  // anything that has to name this node to somebody else - the node list, and the
  // failover work in M4 - needs to be told. Empty means fall back to the bind address,
  // which is right on a single-homed host and useless on a wildcard bind.
  std::string sip_public_address;

  // RFC 4028 section 8.1: the session interval this node puts on a call that asked for
  // none, in seconds. Zero leaves such a call without one.
  //
  // It is what gets an expiry onto a call whose caller never mentioned session timers but
  // whose callee knows what they are - section 9 Table 2 has a timer-aware UAS take the
  // refreshing itself when the UAC cannot. Where neither end implements RFC 4028, section
  // 8.2 leaves the call with no expiration at all and `sip_media_timeout` is what
  // eventually notices.
  //
  // Section 4 recommends 1800 and sets an absolute floor of 90.
  uint32_t sip_session_expires = 1800;

  // RFC 4028 section 8.1: whether to insist on a session timer by putting Require: timer
  // on a request whose caller did not advertise support. Off by default, and it should
  // usually stay off.
  //
  // The RFC calls it NOT RECOMMENDED, and the reason is concrete: an endpoint that does
  // not implement the extension answers 420 Bad Extension, so the call fails outright
  // rather than merely going without an expiry. That is a reasonable trade on a closed
  // fleet where every handset is known, and the wrong one on anything a stranger can
  // call. Where it is off, a caller that cannot do session timers still gets one offered
  // (`sip_session_expires`) and the callee decides.
  bool sip_require_session_timer = false;

  // RFC 4028 section 5: the shortest session interval this node will let a call
  // negotiate, and the floor the RFC itself sets is 90 seconds. A proxy that keeps state
  // for a call has a stake in how often it is told the call is still there, and rejecting
  // an interval below this is the only say it gets (section 8).
  uint32_t sip_session_min_se = 90;

  // SIP Timers
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

  bool sip_timer_reliable_transport_retransmits = false;

  // The longest this node will hold any call open, in seconds. Zero, the default, means
  // no cap at all.
  //
  // The backstop for the calls the other two mechanisms cannot see: one whose media went
  // end to end has nothing for `sip_media_timeout` to watch, and one between two
  // endpoints that neither implement RFC 4028 gets no session timer however willing this
  // node is to offer one. A blunt instrument, which is why it is off - set this and a
  // legitimate call of that length is cut - but on a deployment that knows its calls are
  // never hours long it is the only thing that catches the rest.
  uint32_t sip_max_call_duration = 0;

  // How long a call may carry no media at all before this node stops holding it open,
  // in seconds. Zero turns it off.
  //
  // A call between two endpoints that never negotiated a session timer has no expiry, so
  // a phone that loses power - which sends no BYE - leaves this node holding its dialog,
  // its call record and its relay ports until the process restarts, and the ports are a
  // finite pool. Nothing in the signalling plane will ever say that call ended. The media
  // plane will: the relay knows when it last carried a packet.
  //
  // Minutes rather than seconds, because being wrong means cutting the media on a call
  // that is still up. RTCP counts as well as RTP, so a call on hold or one whose codec
  // suppresses silence is not silent here (RFC 3550 section 6).
  //
  // Only calls this node anchors are covered. Where the engine declined the description,
  // or a realm is set to pass media through, there is nothing to watch.
  uint32_t sip_media_timeout = 300;

  // RFC 3261 16.6 step 11: how long a proxied INVITE branch may go on answering
  // provisionally before this node gives up on it. Timer B does not cover this - the
  // first provisional response moves the client transaction to Proceeding and cancels
  // Timer B (17.1.1.2) - so without timer C a branch that says 180 Ringing and then
  // goes quiet is never given up on at all. The RFC's floor is "larger than 3 minutes";
  // four is comfortably past it and still less than anyone waits for a phone to answer.
  uint32_t sip_timer_c_invite_proxy_ms = 240000;

  // How long this node will spend opening a flow to a next hop it has none to (RFC 3261
  // 16.6 step 7). Not an RFC timer - the RFC does not give one - but it has to be
  // bounded: the operating system's own connect timeout is well over a minute, and Timer
  // B gives the whole transaction thirty-two seconds. A fork with several bindings has
  // to have room to try more than the first.
  uint32_t sip_connect_timeout_ms = 4000;

  // TLS Configuration
  bool tls_enable = false;
  std::string tls_address;
  uint16_t tls_port = 0;
  std::string tls_cert_pem_filename;
  std::string tls_key_pem_filename;

  // TCP Configuration
  bool tcp_enable = false;
  std::string tcp_address;  // e.g. "127.0.0.1"
  uint16_t tcp_port = 0;    // e.g. 5060

  // UDP Configuration
  bool udp_enable = false;
  std::string udp_address;  // e.g. "127.0.0.1"
  uint16_t udp_port = 0;    // e.g. 5060

  // UDP Configuration
  bool websocket_enable = false;
  std::string websocket_address;  // e.g. "127.0.0.1"
  uint16_t websocket_port = 0;    // e.g. 5060

  // RFC 7118 over TLS. A browser will not open an insecure WebSocket from a page served
  // over https, so this is what a web client actually connects to; ws:// is for local
  // development. The certificate is the listener's own rather than the tls section's,
  // because the name a browser reaches the node by is rarely the name a SIP peer does.
  bool websocket_tls = false;
  std::string websocket_cert_pem_filename;
  std::string websocket_key_pem_filename;

  // DB configuration
  // Defaulted, not blank. A node whose configuration says nothing about where to keep
  // state has to start anyway, on what needs no external service - that is the whole of
  // the ten-line config the project promises. redis:// is what a cluster says instead.
  std::string db_url = "memory://";  // or "redis://127.0.0.1:6379"

  // Events configuration
  // Same reason. The event bus carries observability, presence and discovery and is
  // never on the call setup path, so a single node with nothing to tell has a working
  // default and a cluster names its broker.
  std::string events_url = "local://";  // or "mqtt://user:pass@127.0.0.1:1883/athenasip?client_id=sip-01&keep_alive=30"

  // Media configuration
  std::string media_url = "builtin://";  // or "rtpengine://host:port"

  // Built-in RTP relay configuration
  bool rtprelay_enable = false;
  std::string rtprelay_address;
  std::string rtprelay_public_address;
  uint16_t rtprelay_min_port = 22000;
  uint16_t rtprelay_max_port = 23000;

  // HTTP configuration
  std::string http_address;
  uint16_t http_port = 0;  // e.g. 5060
  bool http_api_enable = false;
  bool http_files_enable = false;
  std::string http_files_path;

  // What a bearer token is allowed to do. admin provisions; client reads what a client
  // may see. A token with no scope can do nothing, which is what an empty list means.
  //
  // Tokens live in the config for now. They belong in the datastore once there is
  // anything to administer them with, and the shape here is what that would replace.
  struct ApiToken {
    std::string token;
    std::vector<std::string> scopes;

    bool has_scope(const std::string& scope) const { return std::find(scopes.begin(), scopes.end(), scope) != scopes.end(); }
  };

  std::vector<ApiToken> http_api_tokens;

  // Loads configuration from a YAML file.
  bool load_from_yaml(const std::string& filename);

  // A plugin's own configuration: the section named after the driver, inside the
  // section for its kind. The URL stays the selector, so this is only for what a URL
  // cannot express (engine pools, health-check intervals) and is usually absent.
  //
  //   datastore:
  //     url: redis://127.0.0.1:6379
  //     redis:
  //       health_check_interval: 5
  //
  // Keyed on the driver's name() rather than the scheme it was reached by, so the
  // three Redis schemes share one section instead of needing three.
  YAML::Node plugin_root(const std::string& kind, const std::string& name) const;

 private:
  std::shared_ptr<Logger> _logger;

  // The document as loaded, kept so plugins can be handed their own section. Nothing
  // else should read it: everything the server itself needs is parsed into the fields
  // above at load time.
  YAML::Node _root;
};

}  // namespace athenasip
