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
#include "api/static_middleware.h"
#include "config.h"
#include "databases/db.h"
#include "events/local_event_system.h"
#include "loggers/logger_scoped.h"
#include "loggers/logger_stdio.h"
#include "registrar.h"
#include "rtp/rtp_relay.h"
#include "script/lua_script_engine.h"
#include "servers/tcp_server.h"
#include "servers/tls_server.h"
#include "servers/udp_server.h"
#include "servers/websocket_server.h"
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
  // This is v0.0.1
  auto version = std::make_shared<Version>(0, 0, 1);

  // Create Logger
  auto logger = std::make_shared<loggers::LoggerStdIO>(LogLevel::DEBUG);

  // Log Splash
  logger->raw("-----------------------------------");
  logger->raw(" AthenaSIP v" + version->to_string());
  logger->raw("-----------------------------------");

  // Create Config
  auto config = std::make_shared<Config>(logger);
  // config->load_from_yaml(Util::expand_path("~/.athenasip/config.yaml"));
  config->load_from_yaml(Util::expand_path("/Users/tom/.athenasip/config.yaml"));

  // Create DB
  auto db = databases::DB::create_driver(logger, config->db_database_url);
  if (!db) {
    logger->error("Unknown database scheme");
    return -2;
  }
  if (!db->connect()) {
    logger->error("Database Connection Failed");
    return -3;
  }

  // Events
  std::shared_ptr<events::EventSystem> eventengine;
  // TODO: Other event systems boot here
  if (!eventengine) eventengine = std::make_shared<events::LocalEventSystem>(logger);
  eventengine->start(nullptr);

  // Create Registrar
  auto registrar = std::make_shared<Registrar>(logger, config, db, eventengine);

  // Start RTPRelay
  if (config->rtprelay_enable) {
    auto rtprelay = std::make_shared<rtp::RTPRelay>(logger, config->rtprelay_address, config->rtprelay_min_port, config->rtprelay_max_port);
    registrar->rtprelay_register(rtprelay);
    registrar->rtprelay_start();
  }

  // Script Engine
  auto scripting = std::make_shared<script::LuaScriptEngine>(logger);
  scripting->start();

  // HTTP Admin API
  if (config->http_api_enable || config->http_files_enable) {
    auto adminAPI = std::make_shared<api::AdminAPI>(logger, config->http_address, config->http_port);
    if (config->http_api_enable) {
    }
    if (config->http_files_enable) {
      StaticOptions so;
      adminAPI->middlewares.push_back(api::StaticMiddleware::add("/Users/tom/devroot/athenasip/admin", so));
    }
    adminAPI->middlewares.push_back(api::AdminAPI::send404end());
    registrar->admin_register(adminAPI);
    registrar->admin_start();
  }

  // Servers: Create the TLSServer instance with the logger and start it on the specified port
  if (config->tls_enable) {
    auto tlsServer = std::make_shared<servers::TLSServer>(logger, config->tls_address, config->tls_port);
    // Set Certificates
    if (!tlsServer->set_certificates(config->tls_cert_pem_filename, config->tls_key_pem_filename)) {
      logger->error("Cannot load TLS certificates");
      db->close();
      return -4;
    }
    registrar->server_register(tlsServer);
  }

  // Servers: Create the TCPServer instance with the logger and start it on the specified port
  if (config->tcp_enable) {
    auto tcpServer = std::make_shared<servers::TCPServer>(logger, config->tcp_address, config->tcp_port);
    registrar->server_register(tcpServer);
  }

  // Servers: Create the UDPServer instance with the logger and start it on the specified port
  if (config->udp_enable) {
    auto udpServer = std::make_shared<servers::UDPServer>(logger, config->udp_address, config->udp_port);
    registrar->server_register(udpServer);
  }

  // Servers: Create the Websocket instance with the logger and start it on the specified port
  if (config->websocket_enable) {
    auto websocketServer = std::make_shared<servers::WebsocketServer>(logger, config->websocket_address, config->websocket_port);
    registrar->server_register(websocketServer);
  }

  // Core
  auto core = std::make_shared<SIPCore>(logger, version, config, registrar);

  // Start all configured servers
  registrar->server_start_all(core);

  // Wait for Signals
  boost::asio::io_context signal_wait_context;
  boost::asio::signal_set signals(signal_wait_context, SIGINT, SIGHUP);
  wait_for_signal(signals, signal_wait_context, [&](int signal_number) {
    switch (signal_number) {
      case SIGHUP:
        logger->raw("Received Signal SIGHUP");
        break;
      case SIGINT:
        logger->raw("Received Signal SIGINT");
        signal_wait_context.stop();

        registrar->session_close_all();
        registrar->server_stop_all();
        registrar->rtprelay_stop();
        registrar->admin_stop();

        return;
      default:
        logger->raw("Received Unknown Signal " + std::to_string(signal_number));
    }
  });

  // Do the SIGINT Wait
  signal_wait_context.run();
}
