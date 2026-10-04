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

  std::optional<Finding> finding() const { return _finding; }

  // The stun: servers in http.api.ice_servers, as host and port (RFC 7064; 3478 by default).
  static std::vector<std::pair<std::string, std::uint16_t>> servers_from(const std::vector<std::string>& urls);

 private:
  void _ask(std::size_t index);

  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::weak_ptr<Core> _core;
  std::chrono::seconds _interval{300};

  std::vector<std::pair<std::string, std::uint16_t>> _servers;

  // Transaction id to the server asked, while an answer is awaited.
  std::map<std::string, std::string> _pending;
  std::shared_ptr<Timer> _timer;
  bool _stopped = false;

  std::optional<Finding> _finding;
};

}  // namespace athenasip
