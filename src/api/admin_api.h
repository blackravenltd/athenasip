//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "../loggers/logger.h"
#include "./http_types.h"

namespace athenasip::api {

class AdminAPI {
 public:
  AdminAPI(std::shared_ptr<athenasip::loggers::Logger> logger, const std::string &bind_address, unsigned short port);
  ~AdminAPI();

  using RouteFn = std::function<bool(std::shared_ptr<Request> request, std::shared_ptr<Response> response)>;

  // Non-copyable
  AdminAPI(const AdminAPI &) = delete;
  AdminAPI &operator=(const AdminAPI &) = delete;

  void start();
  void stop();
  void route(std::string route, RouteFn route_fn);

 private:
  void _do_accept();

  std::shared_ptr<athenasip::loggers::Logger> _logger;
  std::string _bind_address;
  unsigned short _port;

  boost::asio::io_context _io_context;
  boost::asio::ip::tcp::acceptor _acceptor;
  std::shared_ptr<std::thread> _thread;
};

}  // namespace athenasip::api
