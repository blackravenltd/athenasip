//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <optional>
#include <string>

#include "config.h"
#include "logger_scoped.h"
#include "logger_stdio.h"
#include "registrar.h"
#include "sip_core.h"
#include "tls_server.h"
#include "util.h"
#include "version.h"

using namespace athenasip;

void wait_for_signal(boost::asio::signal_set& signals, boost::asio::io_context& io_context, std::function<void(int)> onsignal) {
  signals.async_wait([&](const boost::system::error_code& error, int signal_number) {
    if (!error) {
      onsignal(signal_number);
      wait_for_signal(signals, io_context, onsignal);
    }
  });
}

int main(int argc, char* argv[]) {
  // SIP Core Container
  auto core = std::make_shared<SIPCore>();

  // This is v0.1.0
  core->version = std::make_shared<Version>(0, 1, 0);

  // Create Logger
  core->logger = std::make_shared<LoggerStdIO>(LogLevel::DEBUG);

  // Log Splash
  core->logger->raw("-----------------------------------");
  core->logger->raw(" AthenaSIP v" + core->version->to_string());
  core->logger->raw("-----------------------------------");

  // Create Config
  core->config = std::make_shared<Config>(core->logger);

  // Create DB
  core->db = DB::create_driver(core->logger, "mysqlx://root@localhost/athenasip");
  if (!core->db) {
    core->logger->error("Unknown database scheme");
    return -2;
  }
  if (!core->db->connect()) {
    core->logger->error("Database Connection Failed");
    return -3;
  }

  // Create Registrar
  core->registrar = std::make_shared<Registrar>(core->logger, core->db);
  core->registrar->user_exists(std::make_shared<SIPIdentity>("Tom Cully <sip:tom@sip.blackraven.co.nz>"));

  // Create the TLSServer instance with the logger and start it on the specified port
  auto tlsServer = std::make_shared<TLSServer>(core->logger, core->registrar, "tempNonceSecret", 5061);
  tlsServer->set_certificates("../tls/snakeoil.cer", "../tls/snakeoil.key");
  core->server = tlsServer;

  // Set Certificates, Start TCP Server
  core->server->start();

  // Wait for Signals
  boost::asio::io_context signal_wait_context;
  boost::asio::signal_set signals(signal_wait_context, SIGINT, SIGHUP);
  wait_for_signal(signals, signal_wait_context, [&](int signal_number) {
    switch (signal_number) {
      case SIGHUP:
        core->logger->raw("Received Signal SIGHUP");
        break;
      case SIGINT:
        core->logger->raw("Received Signal SIGINT");
        signal_wait_context.stop();
        core->server->stop();
        return;
      default:
        core->logger->raw("Received Unknown Signal " + std::to_string(signal_number));
    }
  });

  // Do the SIGINT Wait
  signal_wait_context.run();
}
