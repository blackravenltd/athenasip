//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>

#include "ioc.h"
#include "logger_stdio.h"
#include "registrar.h"

using namespace athenasip;

#include <mysqlx/xdevapi.h>

#include <boost/asio/signal_set.hpp>
#include <iostream>
#include <optional>
#include <string>

#include "config.h"
#include "logger_scoped.h"
#include "logger_stdio.h"
#include "registrar.h"
#include "tls_server.h"
#include "url.h"
#include "util.h"
#include "version.h"

void wait_for_signal(boost::asio::signal_set& signals, boost::asio::io_context& io_context, std::function<void(int)> onsignal) {
  signals.async_wait([&](const boost::system::error_code& error, int signal_number) {
    if (!error) {
      onsignal(signal_number);
      wait_for_signal(signals, io_context, onsignal);
    }
  });
}

int main(int argc, char* argv[]) {
  // IoC Container
  auto ioc = std::make_shared<IOC>();

  // This is v0.1.0
  ioc->version = std::make_shared<Version>(0, 1, 0);

  // Create Logger
  ioc->logger = std::make_shared<LoggerStdIO>(LogLevel::DEBUG);

  // Log Splash
  ioc->logger->raw("-----------------------------------");
  ioc->logger->raw(" AthenaSIP v" + ioc->version->to_string());
  ioc->logger->raw("-----------------------------------");

  // Create Config
  ioc->config = std::make_shared<Config>(ioc->logger, argc, argv);
  if (!ioc->config->is_valid()) {
    ioc->logger->error("Invalid Configuration");
    return -1;
  }

  // DB
  ioc->db = DB::create_driver(ioc->logger, "mysqlx://root@localhost/athenasip");
  if (!ioc->db) {
    ioc->logger->error("Unknown database scheme");
    return -2;
  }
  if (!ioc->db->connect()) {
    ioc->logger->error("Database Connection Failed");
    return -3;
  }

  // Create Registrar
  ioc->registrar = std::make_shared<Registrar>(ioc->logger, ioc->db);
  ioc->registrar->user_exists(SIPIdentity("Tom Cully <sip:tom@sip.blackraven.co.nz>"));

  // Create the TLSServer instance with the logger and start it on the specified port
  ioc->server = std::make_shared<TLSServer>(ioc->logger, 5061);

  // Set Certificates, Start TCP Server
  ioc->server->set_certificates("../tls/snakeoil.cer", "../tls/snakeoil.key");
  ioc->server->start();

  // Wait for Signals
  boost::asio::io_context signal_wait_context;
  boost::asio::signal_set signals(signal_wait_context, SIGINT, SIGHUP);
  wait_for_signal(signals, signal_wait_context, [&](int signal_number) {
    switch (signal_number) {
      case SIGHUP:
        ioc->logger->raw("Received Signal SIGHUP");
        break;
      case SIGINT:
        ioc->logger->raw("Received Signal SIGINT");
        signal_wait_context.stop();
        ioc->server->stop();
        return;
      default:
        ioc->logger->raw("Received Unknown Signal " + std::to_string(signal_number));
    }
  });

  // Do the SIGINT Wait
  signal_wait_context.run();
}
