//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl.hpp>
#include <iostream>
#include <optional>
#include <string>

#include "loggers/logger_scoped.h"
#include "loggers/logger_stdio.h"
#include "config.h"
#include "datastores/datastore.h"
#include "datastores/datastore_drivers.h"
#include "events/event_system.h"
#include "events/event_system_drivers.h"
#include "registrar.h"
#include "rtp/rtp_relay.h"
#include "script/lua_script_engine.h"
#include "servers/tcp_server.h"
#include "servers/tls_server.h"
#include "servers/udp_server.h"
#include "servers/websocket_server.h"
#include "api/admin_api.h"
#include "api/static_middleware.h"
#include "sip_core.h"
#include "util.h"
#include "version.h"

using namespace athenasip;
using namespace athenasip::datastores;
using namespace athenasip::events;

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

  // Register Datastore Handlers, Event System Handlers
  register_builtin_datastores(logger);
  register_builtin_event_systems(logger);

  // Create Config
  auto config = std::make_shared<Config>(logger);
  // config->load_from_yaml(Util::expand_path("~/.athenasip/config.yaml"));
  config->load_from_yaml(Util::expand_path("~/.athenasip/config.yaml"));

  // Create Datastore
  auto datastore = Datastore::create_driver(logger, config->db_url);
  if (!datastore) {
    logger->error("Unknown datastore scheme: "+config->db_url);
  }

  // Create Event System
  auto events = EventSystem::create_driver(logger, config->events_url);
  if (!events) {
    logger->error("Unknown event scheme: "+config->events_url);
  }

  if(!events || !datastore) {
    if(datastore) datastore->close();
    if(events) events->close();
    return -2;
  }

  logger->info("Datastore Driver: "+datastore->get_driver_name());
  logger->info("Events Driver: "+events->get_driver_name());

  // Attempt Datastore connection
  if (!datastore->connect()) {
    logger->error("Datastore Connection Failed: "+config->db_url);
    return -3;
  }
  // Attempt Events connection
  if (!events->connect()) {
    datastore->close();
    logger->error("Event System Connection Failed: "+config->events_url);
    return -3;
  }

  // Create Registrar
  auto registrar = std::make_shared<Registrar>(logger, config, datastore, events);

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
      adminAPI->middlewares.push_back(api::StaticMiddleware::add("../admin", so));
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
      datastore->close();
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

  // Publish Start Event
  events->publish("athenasip/servers/0001/status","started");

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

        registrar->transaction_end_all();
        registrar->channel_close_all();
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
