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

#include "api/admin_api.h"
#include "config.h"
#include "databases/db.h"
#include "loggers/logger_scoped.h"
#include "loggers/logger_stdio.h"
#include "registrar.h"
#include "rtp/rtp_relay.h"
#include "script/lua_script_engine.h"
#include "servers/tcp_server.h"
#include "servers/tls_server.h"
#include "servers/udp_server.h"
#include "sip_core.h"
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

  // This is v0.0.1
  core->version = std::make_shared<Version>(0, 0, 1);

  // Create Logger
  core->logger = std::make_shared<athenasip::loggers::LoggerStdIO>(LogLevel::DEBUG);

  // Log Splash
  core->logger->raw("-----------------------------------");
  core->logger->raw(" AthenaSIP v" + core->version->to_string());
  core->logger->raw("-----------------------------------");

  // Create Config
  core->config = std::make_shared<Config>(core->logger);
  core->config->load_from_yaml(Util::expand_path("~/.athenasip/config.yaml"));

  // Create DB
  core->db = athenasip::databases::DB::create_driver(core->logger, core->config->db_database_url);
  if (!core->db) {
    core->logger->error("Unknown database scheme");
    return -2;
  }
  if (!core->db->connect()) {
    core->logger->error("Database Connection Failed");
    return -3;
  }

  // Start RTPRelay
  if (core->config->rtprelay_enable) {
    core->rtprelay =
        std::make_shared<rtp::RTPRelay>(core->logger, core->config->rtprelay_address, core->config->rtprelay_min_port, core->config->rtprelay_max_port);
    core->rtprelay->start();
  }

  // Create Registrar
  core->registrar = std::make_shared<Registrar>(core->logger, core->config, core->db, core->rtprelay);

  // Script Engine
  auto lua_scripting = std::make_shared<script::LuaScriptEngine>(core->logger);
  lua_scripting->start();

  // Create the TLSServer instance with the logger and start it on the specified port
  if (core->config->tls_enable) {
    auto tlsServer = std::make_shared<athenasip::servers::TLSServer>(core->logger, core->registrar, core->config->sip_nonce_secret, core->config->tls_address,
                                                                     core->config->tls_port);
    // Set Certificates
    if (!tlsServer->set_certificates(core->config->tls_cert_pem_filename, core->config->tls_key_pem_filename)) {
      core->logger->error("Cannot load TLS certificates");
      core->db->close();
      return -4;
    }
    core->servers.push_back(tlsServer);
    tlsServer->start();
  }

  // Create the TCPServer instance with the logger and start it on the specified port
  if (core->config->tcp_enable) {
    auto tcpServer = std::make_shared<athenasip::servers::TCPServer>(core->logger, core->registrar, core->config->sip_nonce_secret, core->config->tcp_address,
                                                                     core->config->tcp_port);
    core->servers.push_back(tcpServer);
    tcpServer->start();
  }

  // Create the UDPServer instance with the logger and start it on the specified port
  if (core->config->udp_enable) {
    auto udpServer = std::make_shared<athenasip::servers::UDPServer>(core->logger, core->registrar, core->config->sip_nonce_secret, core->config->udp_address,
                                                                     core->config->udp_port);
    core->servers.push_back(udpServer);
    udpServer->start();
  }

  // HTTP Admin API
  auto admin_server = std::make_shared<athenasip::api::AdminAPI>(core->logger, core->config->admin_api_address, core->config->admin_api_port);
  admin_server->start();

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
        core->registrar->session_close_all();
        for (const auto& server : core->servers) server->stop();
        if (core->rtprelay) core->rtprelay->stop();
        return;
      default:
        core->logger->raw("Received Unknown Signal " + std::to_string(signal_number));
    }
  });

  // Do the SIGINT Wait
  signal_wait_context.run();
}
