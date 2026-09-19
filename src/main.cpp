//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl.hpp>
#include <future>
#include <iostream>
#include <optional>
#include <string>

#include "api/admin_api.h"
#include "api/static_middleware.h"
#include "build_version.h"
#include "config.h"
#include "core.h"
#include "datastores/datastore.h"
#include "datastores/datastore_drivers.h"
#include "events/event_system.h"
#include "events/event_system_drivers.h"
#include "events/topics.h"
#include "global_io_context.h"
#include "loggers/logger_scoped.h"
#include "loggers/logger_stdio.h"
#include "media/media_engine.h"
#include "media/media_engine_drivers.h"
#include "plugins/plugin.h"
#include "rtp/rtp_relay.h"
#include "servers/tcp_server.h"
#include "servers/tls_server.h"
#include "servers/udp_server.h"
#include "servers/websocket_server.h"
#include "util.h"
#include "version.h"

using namespace athenasip;
using namespace athenasip::datastores;
using namespace athenasip::events;

// Startup is the one place a wait is the right answer: this is the main thread, there
// is no strand yet, and there is nothing to serve until the datastore answers. The
// contract is async so that the Core strand never waits; main is not the Core strand.
plugins::Status connect_and_wait(const std::function<void(plugins::Executor, plugins::StatusHandler)>& start) {
  std::promise<plugins::Status> promise;
  auto future = promise.get_future();

  start(athenasip::detail::get_global_io_context().get_executor(), [&promise](plugins::Status status) { promise.set_value(std::move(status)); });

  return future.get();
}

// Every plugin is configured the same way, whatever it plugs into: its own section of
// the config, then the whole config for what it cannot be told twice.
bool configure_plugin(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<plugins::Plugin> plugin, std::shared_ptr<Config> config) {
  if (plugin->configure(config->plugin_root(plugin->kind(), plugin->name()), *config)) {
    return true;
  }

  logger->error("Configuration rejected by " + plugin->kind() + " driver " + plugin->describe());
  return false;
}

void wait_for_signal(boost::asio::signal_set& signals, std::function<void(int)> onsignal) {
  signals.async_wait([&signals, onsignal = std::move(onsignal)](const boost::system::error_code& error, int signal_number) mutable {
    if (!error) {
      onsignal(signal_number);
      wait_for_signal(signals, std::move(onsignal));
    }
  });
}

int main(int argc, char* argv[]) {
  auto version = std::make_shared<Version>(ATHENA_VERSION_MAJOR, ATHENA_VERSION_MINOR, ATHENA_VERSION_PATCH);

  // Create Logger
  auto logger = std::make_shared<loggers::LoggerStdIO>(LogLevel::DEBUG);

  // Log Splash
  auto title = " AthenaSIP v" + version->to_string() + " ";
  auto lines = std::string(title.size(), '-');
  logger->raw(lines);
  logger->raw(title);
  logger->raw(lines);

  // Register Datastore Handlers, Event System Handlers
  register_builtin_datastores(logger);
  register_builtin_event_systems(logger);
  media::register_builtin_media_engines(logger);

  // Create Config
  auto config = std::make_shared<Config>(logger);
  const auto config_path = Util::expand_path("~/.athenasip/config.yaml");

  if (!config->load_from_yaml(config_path)) {
    logger->error("Cannot load configuration from " + config_path.string());
    return -1;
  }

  // Create Datastore
  auto datastore = Datastore::create_driver(logger, config->db_url);
  if (!datastore) {
    logger->error("Unknown datastore scheme: " + config->db_url);
  }

  // Create Event System
  auto events = EventSystem::create_driver(logger, config->events_url);
  if (!events) {
    logger->error("Unknown event scheme: " + config->events_url);
  }

  if (!events || !datastore) {
    if (datastore) datastore->close();
    if (events) events->close();
    return -2;
  }

  logger->info("Datastore Driver: " + datastore->describe());
  logger->info("Events Driver: " + events->describe());

  // Configure before connect: a driver reads its own section here, and refusing it is
  // how a driver says the configuration it was given cannot work.
  if (!configure_plugin(logger, datastore, config) || !configure_plugin(logger, events, config)) {
    datastore->close();
    events->close();
    return -2;
  }

  // Attempt Datastore connection
  const auto datastore_connected =
      connect_and_wait([&datastore](plugins::Executor on, plugins::StatusHandler handler) { datastore->connect(std::move(on), std::move(handler)); });

  if (!datastore_connected.ok) {
    logger->error("Datastore Connection Failed: " + config->db_url + " - " + datastore_connected.error);
    return -3;
  }
  // Attempt Events connection
  if (!events->connect()) {
    datastore->close();
    logger->error("Event System Connection Failed: " + config->events_url);
    return -3;
  }

  // Create Media Engine
  auto media_engine = media::MediaEngine::create_driver(logger, config->media_url);
  if (!media_engine) {
    logger->error("Unknown media scheme: " + config->media_url);
    datastore->close();
    events->close();
    return -2;
  }

  logger->info("Media Driver: " + media_engine->describe());

  if (!configure_plugin(logger, media_engine, config)) {
    datastore->close();
    events->close();
    return -2;
  }

  if (!media_engine->connect()) {
    logger->error("Media Engine Connection Failed: " + config->media_url);
    datastore->close();
    events->close();
    return -3;
  }

  // Create Core
  auto core = std::make_shared<Core>(logger, config, datastore, events);
  core->media_register(media_engine);

  // Start RTPRelay
  if (config->rtprelay_enable) {
    auto rtprelay = std::make_shared<rtp::RTPRelay>(logger, config->rtprelay_address, config->rtprelay_min_port, config->rtprelay_max_port);
    core->rtprelay_register(rtprelay);
    core->rtprelay_start();
  }

  // Script Engine
  // auto scripting = std::make_shared<script::LuaScriptEngine>(logger);
  // scripting->start();

  // HTTP Admin API
  if (config->http_api_enable || config->http_files_enable) {
    auto adminAPI = std::make_shared<api::AdminAPI>(logger, config->http_address, config->http_port);
    if (config->http_api_enable) {
    }
    if (config->http_files_enable) {
      StaticOptions so;
      adminAPI->middlewares.push_back(api::StaticMiddleware::add("../admin", so));
    }
    adminAPI->middlewares.push_back(api::AdminAPI::send_404_end());
    core->admin_register(adminAPI);
    core->admin_start();
  }

  // Servers: Create the TLSServer instance with the logger and start it on the specified port
  if (config->tls_enable) {
    auto tlsServer = std::make_shared<servers::TLSServer>(logger, core, config->tls_address, config->tls_port);
    // Set Certificates
    if (!tlsServer->set_certificates(config->tls_cert_pem_filename, config->tls_key_pem_filename)) {
      logger->error("Cannot load TLS certificates");
      datastore->close();
      return -4;
    }
    core->server_register(tlsServer);
  }

  // Servers: Create the TCPServer instance with the logger and start it on the specified port
  if (config->tcp_enable) {
    auto tcpServer = std::make_shared<servers::TCPServer>(logger, core, config->tcp_address, config->tcp_port);
    core->server_register(tcpServer);
  }

  // Servers: Create the UDPServer instance with the logger and start it on the specified port
  if (config->udp_enable) {
    auto udpServer = std::make_shared<servers::UDPServer>(logger, core, config->udp_address, config->udp_port);
    core->server_register(udpServer);
  }

  // Servers: Create the Websocket instance with the logger and start it on the specified port
  if (config->websocket_enable) {
    auto websocketServer = std::make_shared<servers::WebsocketServer>(logger, core, config->websocket_address, config->websocket_port);
    core->server_register(websocketServer);
  }

  // Start all configured servers
  core->server_start_all();

  // Publish Start Event
  core->events->publish(events::topics::node_status(config->sip_node_id), "{\"started\":\"" + Util::get_zulu_time() + "\"}");

  // Wait for Signals
  boost::asio::io_context signal_wait_context;
  boost::asio::signal_set signals(signal_wait_context, SIGINT, SIGHUP);
  wait_for_signal(signals, [&](int signal_number) {
    switch (signal_number) {
      case SIGHUP:
        logger->debug("Received Signal SIGHUP");
        break;

      case SIGINT:
        logger->debug("Received Signal SIGINT");

        // These touch strand-confined state and this is the main thread, so they run
        // through the strand and wait rather than reaching in directly.
        core->call_on_strand([&core]() {
          core->transaction_end_all();
          core->channel_close_all();
        });

        core->server_stop_all();
        core->rtprelay_stop();
        core->admin_stop();

        events->publish(events::topics::node_status(config->sip_node_id), "{\"stopped\":\"" + Util::get_zulu_time() + "\"}");

        media_engine->close();
        datastore->close();
        events->close();

        signal_wait_context.stop();
        return;

      default:
        logger->error("Received Unknown Signal " + std::to_string(signal_number));
        break;
    }
  });

  // Do the SIGINT Wait
  signal_wait_context.run();
}
