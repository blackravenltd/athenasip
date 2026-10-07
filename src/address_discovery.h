//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "loggers/logger.h"
#include "loggers/logger_scoped.h"
#include "stun.h"
#include "timer_source.h"

namespace athenasip {

class Core;

// What this node can learn of its own public address, from the stun: entries in http.api.ice_servers. A STUN
// answer is the mapping a router made for the SIP UDP socket: the address is reliable, the port only a guess
// (a symmetric NAT maps each destination differently), so this reports and does not decide what is advertised.
// sip.public_address, when set, always wins.
class AddressDiscovery : public std::enable_shared_from_this<AddressDiscovery> {
 public:
  struct Finding {
    std::string address;
    std::uint16_t port = 0;

    // "stun:<host>:<port>", the server that said so.
    std::string source;
  };

  AddressDiscovery(std::shared_ptr<loggers::Logger> logger, std::weak_ptr<Core> core);

  // Asks now and then every `interval` while the node runs, so a dynamic address is followed. Nothing happens
  // without a stun: server or a UDP listener to ask from.
  void start(std::chrono::seconds interval = std::chrono::seconds(300));
  void stop();

  // A Binding success response that arrived on the SIP UDP socket.
  void answered(const stun::Mapped& mapped);

  // Where a peer node saw a request from this node come from: the received parameter it put on this node's Via
  // (RFC 3581), read off its response. Taken only when it is a public address and no STUN server has answered;
  // two nodes on one LAN see each other's private addresses, which are no answer to where the node is.
  void observed(const std::string& peer, const std::string& address);

  // Not private, shared, loopback, link-local or multicast: an address the world could use.
  static bool is_public(const boost::asio::ip::address& address);

  std::optional<Finding> finding() const { return _finding; }

  // Run on each status publication. Probes each other node's discovered address with an OPTIONS to its UDP port,
  // at most every ten minutes per address, and reads whether a peer has reached this node's. With
  // sip.public_address unset, a finding a peer has reached becomes the address this node advertises; one none
  // has reached is never advertised.
  void review();

  // The nodes whose discovered address this node has reached: node id to address.
  const std::map<std::string, std::string>& reached() const { return _reached; }

  // The nodes that have reached this node's finding.
  const std::vector<std::string>& verified_by() const { return _verified_by; }

  // The other nodes' inter-node listeners this node has tried, and whether it got through.
  std::map<std::string, bool> cluster_probes() const;

  // True once a peer has tried this node's inter-node listener and none has got through: behind symmetric NAT or
  // a connection-pinning balancer, only flows clients opened reach it, and peers must not forward to it.
  bool cluster_unreachable() const { return _cluster_unreachable; }

  // The stun: servers in http.api.ice_servers, as host and port (RFC 7064; 3478 by default).
  static std::vector<std::pair<std::string, std::uint16_t>> servers_from(const std::vector<std::string>& urls);

 private:
  void _ask(std::size_t index);
  void _probe(const std::string& node, const std::string& address, std::uint16_t port);
  void _adopt();
  void _probe_cluster(const std::string& node, const std::string& address, std::uint16_t port);

  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::weak_ptr<Core> _core;
  std::chrono::seconds _interval{300};

  std::vector<std::pair<std::string, std::uint16_t>> _servers;

  // Transaction id to the server asked, while an answer is awaited.
  std::map<std::string, std::string> _pending;
  std::shared_ptr<Timer> _timer;
  bool _stopped = false;

  std::optional<Finding> _finding;

  std::map<std::string, std::pair<std::string, std::chrono::steady_clock::time_point>> _probed;
  std::map<std::string, std::string> _reached;
  std::vector<std::string> _verified_by;

  struct ClusterProbe {
    std::string at;  // address:port
    std::chrono::steady_clock::time_point when;
    std::optional<bool> reached;
  };
  std::map<std::string, ClusterProbe> _cluster_probed;
  bool _cluster_unreachable = false;
  std::string _warned;
};

}  // namespace athenasip
