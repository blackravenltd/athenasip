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
#include "api/events_api.h"
#include "api/provisioning_api.h"
#include "api/router.h"
#include "api/sessions.h"
#include "api/static_middleware.h"
#include "api/subscriber_auth.h"
#include "api/users_api.h"
#include "build_version.h"
#include "cli.h"
#include "cli_add_user.h"
#include "cli_check.h"
#include "cluster_ca.h"
#include "config.h"
#include "config_schema.h"
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
#include "plugins/module_loader.h"
#include "plugins/plugin.h"
#include "plugins/plugin_registry.h"
#include "push/push_service.h"
#include "push/push_service_drivers.h"
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

// Blocks the main thread until a plugin's async connect answers. Only for startup: the
// Core strand never waits.
plugins::Status connect_and_wait(const std::function<void(plugins::Executor, plugins::StatusHandler)>& start) {
  std::promise<plugins::Status> promise;
  auto future = promise.get_future();

  start(athenasip::detail::get_global_io_context().get_executor(), [&promise](plugins::Status status) { promise.set_value(std::move(status)); });

  return future.get();
}

// Hands a plugin its own config section and the whole config.
bool configure_plugin(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<plugins::Plugin> plugin, std::shared_ptr<Config> config) {
  if (plugin->configure(config->plugin_root(plugin->kind(), plugin->name()), *config)) {
    return true;
  }

  logger->error("Configuration rejected by " + plugin->kind() + " driver " + plugin->describe());
  return false;
}

// Reads a password from the terminal with echo off, or from standard input when piped.
// Never from the command line, which other processes can read.
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

  // Usage errors, --version and --help are answered before anything starts.
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

  // The cluster CA needs no configuration and starts nothing.
  if (options.ca_init || !options.ca_node.empty()) {
    auto dir = options.ca_dir;
    if (dir.empty()) {
      const char* home = std::getenv("HOME");
      dir = std::string(home != nullptr ? home : ".") + "/.athenasip/ca";
    }

    const auto made = options.ca_init ? ca::init(dir) : ca::issue_node(dir, options.ca_node, options.sans, options.replace);
    if (!made.ok) {
      std::cerr << "athenasip: " << made.error << "\n";
      return 1;
    }

    std::cout << "Certificate: " << made.certificate << "\nKey:         " << made.key << "\n";
    return 0;
  }

  // An administrative command logs at WARN so its answer is the whole output. A node logs
  // at DEBUG until the configuration sets the level.
  const auto administering = !options.add_user.empty() || !options.reset_password.empty() || options.print_config || options.check || options.list_plugins ||
                             !options.print_schema.empty();

  auto logger = std::make_shared<loggers::LoggerStdIO>(administering ? LogLevel::WARN : LogLevel::DEBUG);

  if (!administering) {
    auto title = " AthenaSIP v" + version->to_string() + " ";
    auto lines = std::string(title.size(), '-');
    logger->raw(lines);
    logger->raw(title);
    logger->raw(lines);
  }

  register_builtin_datastores(logger);
  register_builtin_event_systems(logger);
  media::register_builtin_media_engines(logger);
  push::register_builtin_push_services(logger);

  // The settings are the server's and the drivers' built in, whatever the file says, so no
  // file is read.
  if (!options.print_schema.empty()) {
    const auto settings = all_config_settings();
    std::cout << (options.print_schema == "markdown" ? config_reference_markdown(settings) : config_schema_json(settings));
    return 0;
  }

  auto config = std::make_shared<Config>(logger);

  // Configuration search order: --config, ATHENASIP_CONFIG, /etc/athenasip/config.yaml,
  // ~/.athenasip/config.yaml.
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

  // Apply log.format and log.level. An administrative command keeps its quieter level.
  logger->set_format(config->log_format);
  if (!administering) logger->set_level(config->log_level);

  // --print-config needs only the loaded file: no driver is constructed, so it works when
  // the datastore is unreachable.
  if (options.print_config) {
    std::cout << "# AthenaSIP " << version->to_string() << " effective configuration\n";
    std::cout << "# from " << config_path.string() << ", with defaults resolved\n";
    std::cout << "#\n";
    std::cout << "# What this node would run on.\n";
    std::cout << config->effective_yaml() << "\n";
    return 0;
  }

  // Plugin modules register beside the drivers built in, before anything is constructed.
  const auto modules = plugins::load_modules(logger, config->plugins_path);

  if (options.list_plugins) {
    for (const auto& module : modules) {
      std::cout << (module.loaded ? "loaded   " : "refused  ") << module.path << (module.name.empty() ? "" : " (" + module.name + ")") << ": " << module.detail
                << "\n";
    }
    if (modules.empty()) std::cout << "No plugin modules" << (config->plugins_path.empty() ? " (plugins.path is not set)" : "") << "\n";

    std::cout << "\nDrivers:\n";
    for (const auto& registration : plugins::PluginRegistry::instance().list()) std::cout << "  " << registration.kind << " " << registration.scheme << "://\n";
    return 0;
  }

  // --check tries each thing the node needs, with drivers of its own, and exits.
  if (options.check) {
    auto lines = cli::check(logger, config, connect_and_wait);

    // The configuration loaded, or this would not be reached.
    lines.insert(lines.begin(), cli::CheckLine{true, "configuration", config_path.string()});
    std::cout << cli::report(lines);
    return cli::passed(lines) ? 0 : 1;
  }

  auto datastore = Datastore::create_driver(logger, config->db_url);
  if (!datastore) {
    logger->error("Unknown datastore scheme: " + config->db_url);
  }

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

  // Configure before connect. A driver rejects a configuration it cannot work with.
  if (!configure_plugin(logger, datastore, config) || !configure_plugin(logger, events, config)) {
    datastore->close();
    events->close();
    return -2;
  }

  const auto datastore_connected =
      connect_and_wait([&datastore](plugins::Executor on, plugins::StatusHandler handler) { datastore->connect(std::move(on), std::move(handler)); });

  if (!datastore_connected.ok) {
    logger->error("Datastore Connection Failed: " + config->db_url + " - " + datastore_connected.error);
    return -3;
  }

  // --reset-password and --add-user need only the datastore, then exit. They connect no
  // bus and start no listener, so they are safe to run beside a serving node.
  //
  // With memory:// the datastore dies with the process, so --add-user creates the user and
  // carries on to start the node.
  const bool keeps_nothing = Util::to_lower(types::URL(config->db_url).scheme) == "memory";

  if (!options.reset_password.empty()) {
    const auto password = read_password("New password for " + options.reset_password + ": ", ::isatty(STDIN_FILENO));

    if (password.empty()) {
      std::cerr << "athenasip: no password given, so nothing was changed\n";
      datastore->close();
      return 2;
    }

    const auto result = cli::reset_password(datastore, athenasip::detail::get_global_io_context().get_executor(), options.reset_password, password);
    datastore->close();

    if (!result.ok()) {
      std::cerr << "athenasip: " << result.message << "\n";
      return result.outcome == cli::ResetPasswordResult::Outcome::missing ? 3 : 1;
    }

    std::cout << "New password set for " << result.message << ", and every session it held ended\n";
    return 0;
  }

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
    logger->set_level(config->log_level);
  }

  // The last-will status, which the broker publishes if this node vanishes. A broker takes
  // a will only at connect, so it is set first.
  events->will_set(events::topics::node_status(config->sip_node_id),
                   Core::node_status_json("down", config->sip_node_id, version->to_string(), datastore->describe(), 0, config->events_status_interval));

  const auto events_connected =
      connect_and_wait([&events](plugins::Executor on, plugins::StatusHandler handler) { events->connect(std::move(on), std::move(handler)); });

  if (!events_connected.ok) {
    datastore->close();
    logger->error("Event System Connection Failed: " + config->events_url + " - " + events_connected.error);
    return -3;
  }

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

  // RFC 8599 push services. With none, a REGISTER asking for push is answered 555.
  std::vector<std::shared_ptr<push::PushService>> push_services;
  for (const auto& url : config->push_urls) {
    auto service = push::PushService::create_driver(logger, url);
    const bool ready =
        service && configure_plugin(logger, service, config) &&
        connect_and_wait([&service](plugins::Executor on, plugins::StatusHandler handler) { service->connect(std::move(on), std::move(handler)); }).ok;

    if (!ready) {
      logger->error(service ? "Push service " + url + " did not start" : "Unknown push scheme: " + url);
      for (const auto& started : push_services) started->close();
      media_engine->close();
      datastore->close();
      events->close();
      return -2;
    }

    logger->info("Push Driver: " + service->describe());
    push_services.push_back(service);
  }

  auto core = std::make_shared<Core>(logger, config, datastore, events);
  core->media_register(media_engine);
  for (const auto& service : push_services) core->push_register(service);

  // The HTTP listener: admin API and static files.
  std::shared_ptr<api::Router> api_router;
  std::shared_ptr<api::ProvisioningAPI> provisioning;

  if (config->http_api_enable || config->http_files_enable) {
    auto adminAPI = std::make_shared<api::AdminAPI>(logger, config->http_address, config->http_port);

    // HTTPS that was asked for and cannot start stops the node.
    if (config->http_tls_enable && !adminAPI->tls_enable(config->http_tls_address, config->http_tls_port, config->http_tls_cert(), config->http_tls_key())) {
      logger->error("Could not start the HTTPS listener on " + config->http_tls_address + ":" + std::to_string(config->http_tls_port));
      return 1;
    }

    if (config->http_api_enable) {
      // The API talks to the datastore on its own executor, never the Core strand, so an
      // admin request cannot hold up a call.
      auto bearer = std::make_shared<api::BearerAuth>();

      const auto limit = [](const Config::RateLimit& from) { return api::RateLimiter::Policy{from.burst, from.per_minute}; };

      api::RateLimits limits;
      limits.open = limit(config->http_api_limit_open);
      limits.login_source = limit(config->http_api_limit_login_source);
      limits.login_user = limit(config->http_api_limit_login_user);
      limits.session = limit(config->http_api_limit_session);

      api_router = std::make_shared<api::Router>(bearer, std::make_shared<api::Throttle>(limits));
      provisioning = std::make_shared<api::ProvisioningAPI>(logger, datastore, adminAPI->executor(), config, version->to_string());
      provisioning->register_routes(*api_router);

      // /api/v1/subscriber/{realm}/...: a subscriber's own routes, signed with its SIP credentials (RFC 7616).
      api_router->subscriber_auth_register(std::make_shared<api::SubscriberAuth>(logger, datastore, adminAPI->executor()));
      provisioning->nodes_register(core->nodes(), std::chrono::seconds(config->events_status_interval));
      provisioning->push_register(push_services);

      // Admin logins. Users live in the datastore; with a driver that cannot hold them a
      // login fails as unavailable, not as a wrong password.
      auto sessions = std::make_shared<api::Sessions>(
          logger, datastore, adminAPI->executor(),
          api::Sessions::Lifetimes{static_cast<std::time_t>(config->http_api_session_lifetime), static_cast<std::time_t>(config->http_api_session_idle)});

      // The router resolves session tokens through this, so routes are guarded by role.
      bearer->sessions_register(sessions);

      auto auth = std::make_shared<api::AuthAPI>(logger, sessions);
      auth->register_routes(*api_router);

      auto users = std::make_shared<api::UsersAPI>(logger, datastore, adminAPI->executor(), sessions);
      users->register_routes(*api_router);

      // Live calls, the media engine and /metrics, read from Core on its strand.
      auto calls = std::make_shared<api::CallsAPI>(logger, core, adminAPI->executor());
      calls->register_routes(*api_router);

      // The event bus as Server-Sent Events, on a response the listener holds open.
      auto events_api = std::make_shared<api::EventsAPI>(logger, events, adminAPI->executor());
      events_api->register_routes(*api_router);
      adminAPI->streams_register(api_router->streams());

      adminAPI->middlewares.push_back(api_router->middleware("/api/"));
      adminAPI->middlewares.push_back(api_router->middleware("/metrics"));
    }

    if (config->http_files_enable) {
      StaticOptions so;

      // Without SPA mode an unknown path is a 404.
      if (!config->http_files_spa) so.fallback.clear();

      logger->info("Serving files from " + config->http_files_path + (config->http_files_spa ? " (SPA mode)" : ""));

      adminAPI->middlewares.push_back(api::StaticMiddleware::add(config->http_files_path, so));
    }

    adminAPI->middlewares.push_back(api::AdminAPI::send_404_end());
    core->admin_register(adminAPI);
    core->admin_start();
  }

  // SIP listeners.
  if (config->tls_enable) {
    auto tlsServer = std::make_shared<servers::TLSServer>(logger, core, config->tls_address, config->tls_port);
    if (!tlsServer->set_certificates(config->tls_cert_pem_filename, config->tls_key_pem_filename)) {
      logger->error("Cannot load TLS certificates");
      datastore->close();
      return -4;
    }
    core->server_register(tlsServer);
  }

  // The inter-node listener: mutual TLS, admitting only certificates the cluster CA signed.
  // The same certificates secure the flows this node opens to its peers.
  if (config->cluster_enable) {
    auto clusterServer = std::make_shared<servers::TLSServer>(logger, core, config->cluster_address, config->cluster_port);
    if (!clusterServer->set_certificates(config->cluster_cert, config->cluster_key) || !clusterServer->require_peer_certificates(config->cluster_ca) ||
        !core->cluster_tls_set(config->cluster_ca, config->cluster_cert, config->cluster_key)) {
      logger->error("Cannot load the cluster certificates");
      datastore->close();
      return -4;
    }
    core->server_register(clusterServer);
  }

  if (config->tcp_enable) {
    auto tcpServer = std::make_shared<servers::TCPServer>(logger, core, config->tcp_address, config->tcp_port);
    core->server_register(tcpServer);
  }

  if (config->udp_enable) {
    auto udpServer = std::make_shared<servers::UDPServer>(logger, core, config->udp_address, config->udp_port);
    core->server_register(udpServer);
  }

  if (config->websocket_enable) {
    auto websocketServer = std::make_shared<servers::WebsocketServer>(logger, core, config->websocket_address, config->websocket_port);

    // Never fall back to ws:// when wss was asked for.
    if (config->websocket_tls && !websocketServer->set_certificates(config->websocket_cert_pem_filename, config->websocket_key_pem_filename)) {
      logger->error("Cannot load WebSocket TLS certificates");
      datastore->close();
      return -4;
    }

    core->server_register(websocketServer);

    // websocket.secure_port: a wss listener beside the plain one.
    if (!config->websocket_tls && config->websocket_secure_port != 0) {
      auto secureServer = std::make_shared<servers::WebsocketServer>(logger, core, config->websocket_address, config->websocket_secure_port);

      if (!secureServer->set_certificates(config->websocket_cert(), config->websocket_key())) {
        logger->error("Cannot load the certificates for 'websocket.secure_port': set them in the websocket section or the tls section");
        datastore->close();
        return -4;
      }

      core->server_register(secureServer);
    }
  }

  core->server_start_all();

  // Publish this node's status, retained, on events.status_interval.
  core->version_set(version->to_string());
  core->node_status_start();

  // Expire idle UDP flows.
  core->flow_sweep_start();

  // Ask the stun: servers in http.api.ice_servers where this node is, for --check and the node status.
  core->post([core]() { core->address_discovery()->start(); });

  // SIGINT and SIGTERM (what systemctl stop sends) shut the node down cleanly. SIGHUP is logged and ignored.
  boost::asio::io_context signal_wait_context;
  boost::asio::signal_set signals(signal_wait_context, SIGINT, SIGTERM, SIGHUP);
  wait_for_signal(signals, [&](int signal_number) {
    switch (signal_number) {
      case SIGHUP:
        logger->debug("Received Signal SIGHUP");
        break;

      case SIGINT:
      case SIGTERM:
        logger->info(std::string("Received ") + (signal_number == SIGTERM ? "SIGTERM" : "SIGINT") + " - shutting down");

        // Strand-confined state, reached from the main thread.
        core->call_on_strand([&core]() {
          core->transaction_end_all();
          core->channel_close_all();
        });

        core->server_stop_all();
        core->admin_stop();

        // Replaces the retained status with "stopped".
        core->node_status_stop();

        core->call_on_strand([core]() { core->address_discovery()->stop(); });
        media_engine->close();
        for (const auto& service : push_services) service->close();
        datastore->close();
        events->close();

        signal_wait_context.stop();
        return;

      default:
        logger->error("Received Unknown Signal " + std::to_string(signal_number));
        break;
    }
  });

  signal_wait_context.run();
}
