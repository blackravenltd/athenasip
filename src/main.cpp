//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <termios.h>
#include <unistd.h>

#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/ssl.hpp>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <optional>
#include <string>

#include "api/admin_api.h"
#include "api/auth_api.h"
#include "api/calls_api.h"
#include "api/provisioning_api.h"
#include "api/router.h"
#include "api/sessions.h"
#include "api/static_middleware.h"
#include "api/users_api.h"
#include "build_version.h"
#include "cli.h"
#include "cli_add_user.h"
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
#include "types/url.h"
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

// A password from the terminal, with the echo off so it does not end up in somebody's
// scrollback, or from standard input when that is not a terminal so a script and the
// compose file can pipe one in. Never from the command line: every other process on the
// host can read that.
std::string read_password(const std::string& prompt, bool confirm) {
  const auto read_line = [](std::string& out) -> bool { return static_cast<bool>(std::getline(std::cin, out)); };

  if (!::isatty(STDIN_FILENO)) {
    std::string password;
    if (!read_line(password)) return {};

    return password;
  }

  const auto read_quietly = [&](const std::string& shown, std::string& out) -> bool {
    std::cout << shown << std::flush;

    termios original{};
    if (::tcgetattr(STDIN_FILENO, &original) != 0) return false;

    termios quiet = original;
    quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);

    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) != 0) return false;

    const auto read = read_line(out);

    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
    std::cout << "\n";

    return read;
  };

  std::string password;
  if (!read_quietly(prompt, password)) return {};

  if (confirm) {
    std::string again;
    if (!read_quietly("Again: ", again)) return {};

    if (again != password) {
      std::cerr << "athenasip: the two passwords are not the same\n";
      return {};
    }
  }

  return password;
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

  const auto options = cli::parse(argc, argv);

  // Answered before anything is started, and on stdout rather than through the logger:
  // a person or a script asked a question, and the answer is the whole output.
  if (!options.ok) {
    std::cerr << "athenasip: " << options.error << "\n\n" << cli::usage();
    return 2;
  }

  if (options.version) {
    std::cout << version->to_string() << "\n";
    return 0;
  }

  if (options.help) {
    std::cout << cli::usage();
    return 0;
  }

  // Create Logger
  //
  // An administrative command is a person asking a question at a prompt, often in the
  // middle of an incident, and the answer is the whole output. A node starting up is a
  // service whose log is the record of what it did, so it stays at DEBUG.
  const auto administering = !options.add_user.empty() || options.print_config;

  auto logger = std::make_shared<loggers::LoggerStdIO>(administering ? LogLevel::WARN : LogLevel::DEBUG);

  // Log Splash. Not for an administrative command: raw goes out whatever the level is,
  // and a banner is not an answer to the question that was asked.
  if (!administering) {
    auto title = " AthenaSIP v" + version->to_string() + " ";
    auto lines = std::string(title.size(), '-');
    logger->raw(lines);
    logger->raw(title);
    logger->raw(lines);
  }

  // Register Datastore Handlers, Event System Handlers
  register_builtin_datastores(logger);
  register_builtin_event_systems(logger);
  media::register_builtin_media_engines(logger);

  // Create Config
  auto config = std::make_shared<Config>(logger);

  // Where a node looks for its configuration, in the order a deployment wants: what it
  // was told, then what the environment says, then the system path a package installs
  // to, and last a home directory, which is a person's checkout and not a service.
  const auto config_path = [&]() -> std::filesystem::path {
    if (!options.config.empty()) return Util::expand_path(options.config);

    if (const auto* from_environment = std::getenv("ATHENASIP_CONFIG"); from_environment != nullptr && *from_environment != '\0') {
      return Util::expand_path(from_environment);
    }

    std::error_code ec;
    const std::filesystem::path system_path = "/etc/athenasip/config.yaml";
    if (std::filesystem::exists(system_path, ec)) return system_path;

    return Util::expand_path("~/.athenasip/config.yaml");
  }();

  logger->info("Configuration: " + config_path.string());

  if (!config->load_from_yaml(config_path)) {
    logger->error("Cannot load configuration from " + config_path.string());
    return -1;
  }

  // Answered here rather than earlier, because the whole question is what the file, the
  // search path and the defaults came to between them - which is not known until the file
  // has been read. Nothing is started and no driver is constructed: a node that cannot
  // reach its datastore must still be able to tell you why it is trying to reach that one.
  if (options.print_config) {
    std::cout << "# AthenaSIP " << version->to_string() << " effective configuration\n";
    std::cout << "# from " << config_path.string() << ", with defaults resolved\n";
    std::cout << "#\n";
    std::cout << "# What this node would run on.\n";
    std::cout << config->effective_yaml() << "\n";
    return 0;
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

  // Administration, and then out. Here rather than earlier because it needs the datastore,
  // and here rather than later because it must not connect the bus, start a listener or
  // build a Core: it is meant to be safe to run against a node that is already serving.
  //
  // Except with memory://, whose users live only in this process. A user created and then
  // dropped on exit is no user at all, and a separate process can never reach a running
  // node's memory - so there the same command creates the user in the datastore this
  // process is about to serve from, and carries on starting the node.
  const bool keeps_nothing = Util::to_lower(types::URL(config->db_url).scheme) == "memory";

  if (!options.add_user.empty()) {
    const auto password = read_password("Password for " + options.add_user + ": ", ::isatty(STDIN_FILENO));

    if (password.empty()) {
      std::cerr << "athenasip: no password given, so no user was created\n";
      datastore->close();
      return 2;
    }

    const auto result =
        cli::add_user(datastore, athenasip::detail::get_global_io_context().get_executor(), options.add_user, options.display_name, options.roles, password);

    if (!result.ok()) {
      datastore->close();
      std::cerr << "athenasip: " << result.message << "\n";
      return result.outcome == cli::AddUserResult::Outcome::taken ? 3 : 1;
    }

    const auto roles = options.roles.empty() ? cli::default_roles() : options.roles;

    std::cout << "Created " << result.message << " holding";
    for (const auto& role : roles) std::cout << " " << role;
    std::cout << "\n";

    if (!keeps_nothing) {
      datastore->close();
      return 0;
    }

    std::cout << "memory:// keeps nothing once this process exits, so this node now starts with that user\n" << std::flush;
    logger->set_level(LogLevel::DEBUG);
  }

  // The will, set while the bus is still closed because that is the only time a broker
  // will take one. A node that is killed, loses power or loses its network never gets
  // to publish again, and without this the last retained thing it said would go on
  // claiming it was healthy.
  events->will_set(events::topics::node_status(config->sip_node_id),
                   Core::node_status_json("down", config->sip_node_id, version->to_string(), datastore->describe(), 0));

  // Attempt Events connection
  const auto events_connected =
      connect_and_wait([&events](plugins::Executor on, plugins::StatusHandler handler) { events->connect(std::move(on), std::move(handler)); });

  if (!events_connected.ok) {
    datastore->close();
    logger->error("Event System Connection Failed: " + config->events_url + " - " + events_connected.error);
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

  const auto media_connected =
      connect_and_wait([&media_engine](plugins::Executor on, plugins::StatusHandler handler) { media_engine->connect(std::move(on), std::move(handler)); });

  if (!media_connected.ok) {
    logger->error("Media Engine Connection Failed: " + config->media_url + " - " + media_connected.error);
    datastore->close();
    events->close();
    return -3;
  }

  // Create Core
  auto core = std::make_shared<Core>(logger, config, datastore, events);
  core->media_register(media_engine);

  // HTTP Admin API
  std::shared_ptr<api::Router> api_router;
  std::shared_ptr<api::ProvisioningAPI> provisioning;

  if (config->http_api_enable || config->http_files_enable) {
    auto adminAPI = std::make_shared<api::AdminAPI>(logger, config->http_address, config->http_port);

    if (config->http_api_enable) {
      // The API talks to the datastore on its own executor. It is not on the Core
      // strand and must not be: an admin listing accounts cannot be allowed to hold up
      // a call, which is what the async plugin contract is for.
      auto bearer = std::make_shared<api::BearerAuth>();

      api_router = std::make_shared<api::Router>(bearer);
      provisioning = std::make_shared<api::ProvisioningAPI>(logger, datastore, adminAPI->executor(), config, version->to_string());
      provisioning->register_routes(*api_router);

      // Admin logins. The datastore is what holds the users, so a driver that does not
      // implement the user operations answers that it cannot and a login fails as
      // unavailable rather than as a wrong password.
      auto sessions = std::make_shared<api::Sessions>(
          logger, datastore, adminAPI->executor(),
          api::Sessions::Lifetimes{static_cast<std::time_t>(config->http_api_session_lifetime), static_cast<std::time_t>(config->http_api_session_idle)});

      // The router resolves a session token through this, so every route can name roles
      // rather than scopes and a user's credential works everywhere a token's does.
      bearer->sessions_register(sessions);

      auto auth = std::make_shared<api::AuthAPI>(logger, sessions);
      auth->register_routes(*api_router);

      auto users = std::make_shared<api::UsersAPI>(logger, datastore, adminAPI->executor(), sessions);
      users->register_routes(*api_router);

      // What the node is doing now: live calls, the media engine, and /metrics for a
      // monitoring system. It reads Core's own state, on Core's strand.
      auto calls = std::make_shared<api::CallsAPI>(logger, core, adminAPI->executor());
      calls->register_routes(*api_router);

      adminAPI->middlewares.push_back(api_router->middleware("/api/"));
      adminAPI->middlewares.push_back(api_router->middleware("/metrics"));
    }

    if (config->http_files_enable) {
      StaticOptions so;

      // An ordinary document root rather than a single-page application: a path with
      // nothing behind it is a 404 again, which is what it should be for anything that
      // is not routed in a browser.
      if (!config->http_files_spa) so.fallback.clear();

      logger->info("Serving files from " + config->http_files_path + (config->http_files_spa ? " (SPA mode)" : ""));

      adminAPI->middlewares.push_back(api::StaticMiddleware::add(config->http_files_path, so));
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

    // A listener asked to be secure and unable to be is fatal. Serving a browser over
    // ws:// because the certificate would not load is the failure nobody notices.
    if (config->websocket_tls && !websocketServer->set_certificates(config->websocket_cert_pem_filename, config->websocket_key_pem_filename)) {
      logger->error("Cannot load WebSocket TLS certificates");
      datastore->close();
      return -4;
    }

    core->server_register(websocketServer);
  }

  // Start all configured servers
  core->server_start_all();

  // Say what this node is, and keep saying it. Retained and on an interval, with the
  // broker told what to say if this node stops saying anything at all: a monitor asks
  // "is it alive", and a message published once at startup answers a different question.
  core->version_set(version->to_string());
  core->node_status_start();

  // UDP flows have no socket to end them, so something has to forget the quiet ones.
  core->flow_sweep_start();

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
        core->admin_stop();

        // Replaces the retained heartbeat, so nothing is left claiming for ever that a
        // node which stopped cleanly is still up.
        core->node_status_stop();

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
