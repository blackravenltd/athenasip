//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/bind/bind.hpp>
#include <iostream>
#include <memory>
#include <shared_mutex>
#include <thread>
#include <unordered_map>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "connection.h"

using namespace athenasip;
using namespace athenasip::loggers;

namespace athenasip {
class Core;
}  // namespace athenasip

namespace athenasip::servers {

class Server : public std::enable_shared_from_this<Server> {
 public:
  Server(std::shared_ptr<Logger> logger, std::shared_ptr<Core> core) : _logger(logger), _core(core) {}

  virtual void start() = 0;
  virtual void stop() = 0;

  // Opens a flow to a peer this listener has not heard from. Only a datagram listener can: the datagram must leave by
  // the listener's own socket, the source port the far end answers to (RFC 3261 18.1.1, RFC 3581). Others return false.
  //
  // True means the handler will be called on the Core strand with the connection, or with null. The channel over the
  // connection is the caller's to make.
  virtual bool open_datagram_flow(boost::asio::ip::udp::endpoint remote, std::function<void(std::shared_ptr<Connection>)> handler) {
    (void)remote;
    (void)handler;
    return false;
  }

 protected:
  std::shared_ptr<Logger> _logger;
  std::shared_ptr<Core> _core;
};

}  // namespace athenasip::servers
