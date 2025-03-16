//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <memory>
#include <set>
#include <sstream>
#include <string>

#include "../global_io_context.h"
#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "rtp_relay_set.h"

namespace athenasip::rtp {
class RTPRelay : public std::enable_shared_from_this<RTPRelay> {
 public:
  RTPRelay(std::shared_ptr<Logger> logger, const std::string& bind_address, uint16_t min_port, uint16_t max_port)
      : _logger(std::make_shared<LoggerScoped>("rtprelay", logger)), _bind_address(bind_address), _min_port(min_port), _max_port(max_port) {}

  void start() {
    for (int i = _min_port; i < _max_port; i++) _available_ports.insert(i);
    _logger->info("Started on " + _bind_address + ", Available Ports " + std::to_string(_min_port) + " - " + std::to_string(_max_port));
  }

  void stop() {
    for (auto& relay : _relays) {
      relay->stop();
    }
    _relays.clear();
    _available_ports.clear();
    _allocated_ports.clear();
    _logger->info("Stopped");
  }

  std::shared_ptr<RTPRelaySet> allocate_relay_set() {
    auto port = _allocate_port();

    if (port == 0) {
      _logger->error("Available ports exhausted");
      return nullptr;
    }

    auto relay = std::make_shared<RTPRelaySet>(_logger, _bind_address, port);
    _relays.insert(relay);
    return relay;
  }

  void release_relay_set(std::shared_ptr<RTPRelaySet> relay) {
    relay->stop();
    _release_port(relay->port);
    _relays.erase(relay);
  }

 private:
  std::shared_ptr<Logger> _logger;
  std::string _bind_address;

  uint16_t _min_port, _max_port;
  std::set<uint16_t> _available_ports;
  std::set<uint16_t> _allocated_ports;

  std::set<std::shared_ptr<RTPRelaySet>> _relays;

  uint16_t _allocate_port() {
    if (_available_ports.size() == 0) return 0;
    auto node = _available_ports.extract(_available_ports.begin());
    _allocated_ports.insert(node.value());
    return node.value();
  }

  void _release_port(uint16_t port) {
    if (port == 0) return;
    _allocated_ports.erase(port);
    _available_ports.insert(port);
  }
};

}  // namespace athenasip::rtp
