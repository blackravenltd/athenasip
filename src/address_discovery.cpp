//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "address_discovery.h"

#include <openssl/rand.h>

#include <boost/asio/post.hpp>
#include <chrono>
#include <utility>

#include "channel.h"
#include "core.h"
#include "node_directory.h"
#include "sip_message.h"
#include "util.h"

namespace athenasip {

namespace {

// RFC 5389 7.2.1 retransmits for 39.5 seconds; one server not answering in three is enough to try the next.
constexpr auto kAnswerWithin = std::chrono::seconds(3);

}  // namespace

AddressDiscovery::AddressDiscovery(std::shared_ptr<loggers::Logger> logger, std::weak_ptr<Core> core)
    : _logger(std::make_shared<loggers::LoggerScoped>("address", std::move(logger))), _core(std::move(core)) {}

std::vector<std::pair<std::string, std::uint16_t>> AddressDiscovery::servers_from(const std::vector<std::string>& urls) {
  std::vector<std::pair<std::string, std::uint16_t>> servers;

  for (const auto& url : urls) {
    if (Util::to_lower(url.substr(0, 5)) != "stun:") continue;

    // RFC 7064: stun:host[:port], the host possibly an IPv6 literal in brackets.
    auto rest = url.substr(5);
    if (const auto query = rest.find('?'); query != std::string::npos) rest = rest.substr(0, query);

    std::string host = rest;
    std::uint16_t port = 3478;

    const auto bracket = rest.find(']');
    const auto colon = rest.rfind(':');
    if (colon != std::string::npos && (bracket == std::string::npos || colon > bracket)) {
      host = rest.substr(0, colon);
      try {
        port = static_cast<std::uint16_t>(std::stoul(rest.substr(colon + 1)));
      } catch (const std::exception&) {
        continue;
      }
    }

    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    if (!host.empty()) servers.emplace_back(host, port);
  }

  return servers;
}

void AddressDiscovery::start(std::chrono::seconds interval) {
  auto core = _core.lock();
  if (!core) return;

  std::vector<std::string> urls;
  for (const auto& server : core->config->ice_servers) urls.push_back(server.url);

  _servers = servers_from(urls);
  _interval = interval;
  _stopped = false;

  if (_servers.empty() || !core->config->udp_enable) return;
  _ask(0);
}

void AddressDiscovery::stop() {
  _stopped = true;
  if (_timer) _timer->cancel();
  _timer.reset();
  _pending.clear();
}

void AddressDiscovery::_ask(std::size_t index) {
  auto core = _core.lock();
  if (!core || _stopped) return;

  auto timers = core->timer_source();
  if (!timers) return;

  std::weak_ptr<AddressDiscovery> weak_self = weak_from_this();
  auto strand = core->strand();
  auto later = [weak_self, strand](std::size_t next) {
    return [weak_self, strand, next]() {
      boost::asio::post(strand, [weak_self, next]() {
        if (auto self = weak_self.lock()) self->_ask(next);
      });
    };
  };

  // Every server asked this round: ask again after the interval, from the first.
  if (index >= _servers.size()) {
    _timer = timers->schedule(std::chrono::duration_cast<std::chrono::milliseconds>(_interval), later(0));
    return;
  }

  const auto [host, port] = _servers[index];
  const auto source = "stun:" + host + ":" + std::to_string(port);

  std::string transaction(12, '\0');
  RAND_bytes(reinterpret_cast<unsigned char*>(transaction.data()), static_cast<int>(transaction.size()));

  auto self = shared_from_this();

  // Sent from the SIP UDP socket: its mapping is the one clients and peers reach.
  core->channel_connect("udp", host, port, [this, self, index, source, transaction, timers](plugins::Result<std::shared_ptr<Channel>> opened) {
    if (_stopped) return;

    if (!opened.ok || !opened.value) {
      _logger->info("Cannot ask " + source + " this node's address - " + opened.error);
      return _ask(index + 1);
    }

    _pending[transaction] = source;
    opened.value->write(stun::binding_request(transaction));

    // An unanswered request is given up, and the next server asked.
    std::weak_ptr<AddressDiscovery> weak_self = weak_from_this();
    auto core = _core.lock();
    if (!core) return;
    _timer = timers->schedule(kAnswerWithin, [weak_self, strand = core->strand(), transaction, index]() {
      boost::asio::post(strand, [weak_self, transaction, index]() {
        auto self = weak_self.lock();
        if (self && self->_pending.erase(transaction) != 0) self->_ask(index + 1);
      });
    });
  });
}

void AddressDiscovery::answered(const stun::Mapped& mapped) {
  auto found = _pending.find(mapped.transaction_id);
  if (found == _pending.end()) return;

  const Finding finding{mapped.address.to_string(), mapped.port, found->second};
  _pending.erase(found);

  if (!_finding || _finding->address != finding.address || _finding->port != finding.port) {
    _logger->info(finding.source + " sees this node at " + finding.address + ":" + std::to_string(finding.port));
  }
  _finding = finding;

  // The round is done: ask again after the interval.
  if (_timer) _timer->cancel();
  _ask(_servers.size());
}

void AddressDiscovery::review() {
  auto core = _core.lock();
  if (!core) return;

  const auto interval = core->config->events_status_interval;
  const auto stale_after = interval == 0 ? std::chrono::seconds::max() : std::chrono::seconds(interval * 3);
  const auto& self_id = core->config->sip_node_id;
  const auto now = core->now();

  _verified_by.clear();
  bool peers = false;
  bool probed = false;
  bool reached_cluster = false;

  for (const auto& node : core->nodes()->list(stale_after)) {
    if (node.id == self_id || node.stale || node.status != "ok") continue;
    peers = true;

    // Whether this peer got through to the address this node found, and to its inter-node listener.
    for (const auto& [reached, address] : node.reaches) {
      if (_finding && reached == self_id && address == _finding->address) _verified_by.push_back(node.id);
    }
    for (const auto& [target, reached] : node.cluster_probes) {
      if (target != self_id) continue;
      probed = true;
      reached_cluster = reached_cluster || reached;
    }

    // Try the peer's inter-node listener, as forwarding would, every ten minutes.
    if (core->config->cluster_enable && !node.cluster_address.empty() && node.cluster_port != 0) {
      const auto at = node.cluster_address + ":" + std::to_string(node.cluster_port);
      const auto previous = _cluster_probed.find(node.id);
      if (previous == _cluster_probed.end() || previous->second.at != at || now - previous->second.when >= std::chrono::minutes(10)) {
        _probe_cluster(node.id, node.cluster_address, node.cluster_port);
      }
    }

    // Probe the peer's own finding, at its UDP listener's port.
    if (node.discovered.empty()) continue;

    std::uint16_t port = 0;
    for (const auto& entry : node.transports) {
      if (!entry.is_object()) continue;
      const auto* kind = entry.as_object().if_contains("transport");
      const auto* at = entry.as_object().if_contains("port");
      if (kind != nullptr && kind->is_string() && kind->as_string() == "udp" && at != nullptr && at->is_int64())
        port = static_cast<std::uint16_t>(at->as_int64());
    }
    if (port == 0) continue;

    const auto probed = _probed.find(node.id);
    if (probed != _probed.end() && probed->second.first == node.discovered && now - probed->second.second < std::chrono::minutes(10)) continue;

    _probe(node.id, node.discovered, port);
  }

  const bool unreachable = core->config->cluster_enable && probed && !reached_cluster;
  if (unreachable != _cluster_unreachable) {
    _cluster_unreachable = unreachable;
    if (unreachable) {
      _logger->warn("No other node can reach this node's inter-node listener; it is marked unreachable and is not forwarded to");
    } else {
      _logger->warn("Another node reaches this node's inter-node listener again");
    }
  }

  if (_finding && _verified_by.empty() && peers && core->config->sip_public_address.empty() && _warned != _finding->address) {
    _warned = _finding->address;
    _logger->warn("No other node has reached this node at " + _finding->address + ", so it is not advertised; set sip.public_address");
  }

  _adopt();
}

void AddressDiscovery::_probe(const std::string& node, const std::string& address, std::uint16_t port) {
  auto core = _core.lock();
  if (!core) return;

  _probed[node] = {address, core->now()};
  auto self = shared_from_this();

  core->channel_connect("udp", address, port, [this, self, node, address, port](plugins::Result<std::shared_ptr<Channel>> opened) {
    auto core = _core.lock();
    if (!core || _stopped) return;

    if (!opened.ok || !opened.value || !opened.value->_connection) {
      _reached.erase(node);
      return;
    }

    // RFC 3261 11.1: an OPTIONS for the node itself, which it answers (11.2).
    const auto channel = opened.value;
    const auto advertised = core->advertised_for(*channel);
    const auto branch = std::string("z9hG4bK") + Util::generate_random_string("", 16);

    std::string raw = "OPTIONS sip:" + address + ":" + std::to_string(port) + " SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP " + advertised.host + ":" + std::to_string(advertised.port) + ";branch=" + branch + ";rport\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "From: <sip:athenasip@" + advertised.host + ">;tag=" + Util::generate_random_string("", 10) + "\r\n";
    raw += "To: <sip:" + address + ":" + std::to_string(port) + ">\r\n";
    raw += "Call-ID: " + Util::generate_random_string("", 20) + "@" + core->config->sip_node_id + "\r\n";
    raw += "CSeq: 1 OPTIONS\r\n";
    raw += "Content-Length: 0\r\n";

    auto request = std::make_shared<SIPMessage>();
    request->header = std::make_shared<SIPHeader>(raw);
    request->branch = branch;

    std::weak_ptr<AddressDiscovery> weak_self = weak_from_this();
    core->client_transaction_start(
        request, channel,
        [weak_self, node, address](std::shared_ptr<SIPMessage> response) {
          auto self = weak_self.lock();
          if (!self || response->header->response_code < 200) return;
          if (response->header->response_code < 300) {
            self->_reached[node] = address;
          } else {
            self->_reached.erase(node);
          }
        },
        [weak_self, node]() {
          if (auto self = weak_self.lock()) self->_reached.erase(node);
        });
  });
}

void AddressDiscovery::_adopt() {
  auto core = _core.lock();
  if (!core || !core->config->sip_public_address.empty()) return;

  const auto chosen = _finding && !_verified_by.empty() ? _finding->address : std::string();
  if (chosen == core->config->public_address()) return;

  core->config->discovered_address_set(chosen);

  if (chosen.empty()) {
    _logger->warn("No longer advertising a discovered address: no other node reaches it");
    return;
  }

  _logger->warn("Advertising " + chosen + ", which " + _finding->source + " reported and node " + _verified_by.front() + " reached");

  // A Route naming the new address is this node (RFC 3261 16.4).
  for (const auto& transport : core->config->advertised_transports()) core->local_address_add(transport.address + ":" + std::to_string(transport.port));
}

std::map<std::string, bool> AddressDiscovery::cluster_probes() const {
  std::map<std::string, bool> out;
  for (const auto& [node, probe] : _cluster_probed) {
    if (probe.reached) out[node] = *probe.reached;
  }
  return out;
}

// The mutual TLS flow forwarding would use (Core::channel_connect); a flow already open counts.
void AddressDiscovery::_probe_cluster(const std::string& node, const std::string& address, std::uint16_t port) {
  auto core = _core.lock();
  if (!core) return;

  auto& probe = _cluster_probed[node];
  probe.at = address + ":" + std::to_string(port);
  probe.when = core->now();

  auto self = shared_from_this();
  core->channel_connect("tls", address, port, [this, self, node, at = probe.at](plugins::Result<std::shared_ptr<Channel>> opened) {
    auto found = _cluster_probed.find(node);
    if (found == _cluster_probed.end() || found->second.at != at) return;
    found->second.reached = opened.ok && opened.value != nullptr;
  });
}

}  // namespace athenasip
