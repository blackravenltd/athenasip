//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "cli_check.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/ssl.hpp>
#include <chrono>
#include <mutex>
#include <thread>

#include "address_discovery.h"
#include "datastores/datastore.h"
#include "events/event_system.h"
#include "events/topics.h"
#include "global_io_context.h"
#include "media/media_engine.h"
#include "node_directory.h"
#include "servers/tls_context.h"
#include "stun.h"
#include "types/url.h"

namespace athenasip::cli {

namespace {

using boost::asio::ip::tcp;

CheckLine line(bool ok, std::string what, std::string detail) { return CheckLine{ok, std::move(what), std::move(detail)}; }

// Creates, configures and connects a plugin as main does, then closes it.
template <typename Plugin>
CheckLine check_plugin(const std::string& what, const std::string& url, std::shared_ptr<Plugin> plugin, const std::shared_ptr<Config>& config,
                       const ConnectAndWait& connect_and_wait, bool keep_open = false) {
  if (!plugin) return line(false, what, "no driver for " + redacted(url));

  const auto where = plugin->describe() + " at " + redacted(url);

  if (!plugin->configure(config->plugin_root(plugin->kind(), plugin->name()), *config))
    return line(false, what, where + " - the driver refused its configuration");

  const auto connected =
      connect_and_wait([&plugin](plugins::Executor on, plugins::StatusHandler handler) { plugin->connect(std::move(on), std::move(handler)); });
  if (!keep_open || !connected.ok) plugin->close();

  return connected.ok ? line(true, what, where) : line(false, what, where + " - " + connected.error);
}

// Loads a certificate and key the way the listener that uses them does.
CheckLine check_certificate(const std::shared_ptr<loggers::Logger>& logger, const std::string& what, const std::string& cert, const std::string& key) {
  boost::asio::ssl::context context(boost::asio::ssl::context::tls_server);

  return servers::load_tls_certificates(logger, context, cert, key)
             ? line(true, what, cert)
             : line(false, what,
                    (cert.empty() ? std::string("no certificate") : cert) + " - cannot be loaded with " + (key.empty() ? std::string("no key") : key));
}

// A mutual TLS handshake with a peer's inter-node listener: this node's certificate is
// presented, and the peer's is verified against the cluster CA and the dialled address.
CheckLine check_peer(const std::shared_ptr<loggers::Logger>& logger, const Config& config, const NodeDirectory::Node& node) {
  const auto what = "peer " + node.id;
  const auto where = node.cluster_address + ":" + std::to_string(node.cluster_port);

  boost::asio::ssl::context context(boost::asio::ssl::context::tls_client);
  if (!servers::load_tls_certificates(logger, context, config.cluster_cert, config.cluster_key) ||
      !servers::require_peer_certificates(logger, context, config.cluster_ca)) {
    return line(false, what, where + " - this node's cluster certificates cannot be loaded");
  }

  boost::asio::io_context io;
  boost::asio::ssl::stream<tcp::socket> stream(io, context);
  stream.set_verify_callback(boost::asio::ssl::host_name_verification(node.cluster_address));

  boost::system::error_code failed = boost::asio::error::timed_out;
  tcp::resolver resolver(io);

  resolver.async_resolve(node.cluster_address, std::to_string(node.cluster_port), [&](boost::system::error_code ec, tcp::resolver::results_type results) {
    if (ec) {
      failed = ec;
      return;
    }

    boost::asio::async_connect(stream.next_layer(), results, [&](boost::system::error_code ec, const tcp::endpoint&) {
      if (ec) {
        failed = ec;
        return;
      }

      stream.async_handshake(boost::asio::ssl::stream_base::client, [&](boost::system::error_code ec) { failed = ec; });
    });
  });

  io.run_for(std::chrono::seconds(5));

  boost::system::error_code ignored;
  stream.next_layer().close(ignored);

  return failed ? line(false, what, where + " - " + failed.message()) : line(true, what, where + ", mutual TLS");
}

}  // namespace

std::string redacted(const std::string& url) {
  types::URL parsed(url);
  if (!parsed.password) return url;

  parsed.password = "***";
  return parsed.to_string();
}

namespace {

// What the stun: servers in http.api.ice_servers say this node's address is, against sip.public_address. Asked
// from a socket of its own, because the node may be running and holding the SIP port, so only the address is
// reported.
CheckLine check_address(const Config& config) {
  std::vector<std::string> urls;
  for (const auto& server : config.ice_servers) urls.push_back(server.url);
  const auto servers = AddressDiscovery::servers_from(urls);

  const auto& configured = config.sip_public_address;
  if (servers.empty()) {
    return line(true, "public address",
                configured.empty() ? "not configured, and no stun: server in http.api.ice_servers to ask" : configured + " (sip.public_address)");
  }

  for (const auto& [host, port] : servers) {
    boost::asio::io_context io;
    boost::system::error_code ec;

    boost::asio::ip::udp::resolver resolver(io);
    const auto endpoints = resolver.resolve(host, std::to_string(port), ec);
    if (ec || endpoints.empty()) continue;
    const auto server = *endpoints.begin();

    boost::asio::ip::udp::socket socket(io);
    socket.open(server.endpoint().protocol(), ec);
    if (ec) continue;

    const std::string transaction = "athenasipchk";
    socket.send_to(boost::asio::buffer(stun::binding_request(transaction)), server.endpoint(), 0, ec);
    if (ec) continue;

    std::string answer(2048, '\0');
    boost::asio::ip::udp::endpoint from;
    std::optional<stun::Mapped> mapped;
    socket.async_receive_from(boost::asio::buffer(answer), from, [&](const boost::system::error_code& received, std::size_t size) {
      if (received) return;
      answer.resize(size);
      mapped = stun::binding_success(answer);
    });
    io.run_for(std::chrono::seconds(3));

    if (!mapped || mapped->transaction_id != transaction) continue;

    const auto seen = mapped->address.to_string();
    const auto source = "stun:" + host + ":" + std::to_string(port);

    if (configured.empty()) return line(true, "public address", seen + " according to " + source + "; set sip.public_address to advertise it");

    // sip.public_address may be a name: it matches if it resolves to what the server saw.
    bool matches = configured == seen;
    if (!matches) {
      for (const auto& entry : resolver.resolve(configured, "", ec)) matches = matches || entry.endpoint().address() == mapped->address;
    }

    if (matches) return line(true, "public address", configured + ", as " + source + " sees it");
    return line(false, "public address", "sip.public_address is " + configured + " but " + source + " sees " + seen);
  }

  return line(false, "public address", "no stun: server in http.api.ice_servers answered");
}

}  // namespace

std::vector<CheckLine> check(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Config> config, const ConnectAndWait& connect_and_wait) {
  std::vector<CheckLine> lines;

  lines.push_back(check_plugin("datastore", config->db_url, datastores::Datastore::create_driver(logger, config->db_url), config, connect_and_wait));

  // Node statuses are retained on the bus, so listening briefly discovers the peers.
  auto nodes = std::make_shared<NodeDirectory>();
  auto events = events::EventSystem::create_driver(logger, config->events_url);
  lines.push_back(check_plugin("events", config->events_url, events, config, connect_and_wait, true));

  if (events && lines.back().ok) {
    events->subscribe(
        detail::get_global_io_context().get_executor(), events::topics::node_status("+"),
        [nodes](std::string topic, std::string message) { nodes->observe(topic, message); }, [](plugins::Result<std::shared_ptr<events::Subscription>>) {});

    if (config->cluster_enable) std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    events->close();
  }

  lines.push_back(check_plugin("media", config->media_url, media::MediaEngine::create_driver(logger, config->media_url), config, connect_and_wait));

  // Every certificate a listener will load, so a missing file is found before a client is.
  if (config->tls_enable) lines.push_back(check_certificate(logger, "tls certificate", config->tls_cert_pem_filename, config->tls_key_pem_filename));
  if (config->websocket_enable && (config->websocket_tls || config->websocket_secure_port != 0)) {
    lines.push_back(check_certificate(logger, "websocket certificate", config->websocket_cert(), config->websocket_key()));
  }
  if (config->http_tls_enable) lines.push_back(check_certificate(logger, "http certificate", config->http_tls_cert(), config->http_tls_key()));

  if (config->cluster_enable) {
    lines.push_back(check_certificate(logger, "cluster certificate", config->cluster_cert, config->cluster_key));

    std::size_t peers = 0;
    for (const auto& node : nodes->list(std::chrono::seconds::max())) {
      if (node.id == config->sip_node_id || node.status != "ok" || node.cluster_address.empty() || node.cluster_port == 0) continue;

      lines.push_back(check_peer(logger, *config, node));
      peers++;
    }

    // Not a failure: the first node of a cluster has no peers.
    if (peers == 0) lines.push_back(line(true, "peers", "no other node has said it is up on the event bus"));
  }

  lines.push_back(check_address(*config));

  return lines;
}

std::string report(const std::vector<CheckLine>& lines) {
  std::size_t width = 0;
  for (const auto& entry : lines) width = std::max(width, entry.what.size());

  std::string out;
  for (const auto& entry : lines) {
    out += entry.ok ? "ok    " : "FAIL  ";
    out += entry.what + std::string(width - entry.what.size() + 2, ' ') + entry.detail + "\n";
  }
  return out;
}

bool passed(const std::vector<CheckLine>& lines) {
  for (const auto& entry : lines) {
    if (!entry.ok) return false;
  }
  return true;
}

}  // namespace athenasip::cli
