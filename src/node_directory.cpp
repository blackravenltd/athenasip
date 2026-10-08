//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "node_directory.h"

#include <utility>

namespace athenasip {

namespace {

constexpr std::string_view kPrefix = "nodes/";
constexpr std::string_view kSuffix = "/status";

// The node id in a nodes/<id>/status topic, or empty for any other topic.
std::string node_of(const std::string& topic) {
  if (topic.size() <= kPrefix.size() + kSuffix.size()) return "";
  if (topic.compare(0, kPrefix.size(), kPrefix) != 0) return "";
  if (topic.compare(topic.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) return "";

  auto id = topic.substr(kPrefix.size(), topic.size() - kPrefix.size() - kSuffix.size());
  return id.find('/') == std::string::npos ? id : "";
}

std::string string_of(const boost::json::object& object, const char* key) {
  const auto* value = object.if_contains(key);
  return value != nullptr && value->is_string() ? std::string(value->as_string()) : std::string();
}

}  // namespace

bool NodeDirectory::observe(const std::string& topic, const std::string& message, std::chrono::steady_clock::time_point now) {
  const auto id = node_of(topic);
  if (id.empty()) return false;

  boost::system::error_code error;
  auto parsed = boost::json::parse(message, error);
  if (error || !parsed.is_object()) return false;

  const auto& report = parsed.as_object();

  Node node;
  node.id = id;
  node.status = string_of(report, "status");
  node.version = string_of(report, "version");
  node.at = string_of(report, "at");
  node.heard = now;

  if (node.status.empty() || string_of(report, "node") != id) return false;

  if (const auto* transports = report.if_contains("transports"); transports != nullptr && transports->is_array()) node.transports = transports->as_array();

  if (const auto* cluster = report.if_contains("cluster"); cluster != nullptr && cluster->is_object()) {
    const auto& peer = cluster->as_object();
    const auto* port = peer.if_contains("port");

    node.cluster_address = string_of(peer, "address");
    if (const auto* reachable = peer.if_contains("reachable"); reachable != nullptr && reachable->is_bool()) node.cluster_reachable = reachable->as_bool();
    if (port != nullptr && port->is_int64() && port->as_int64() > 0 && port->as_int64() <= 65535)
      node.cluster_port = static_cast<std::uint16_t>(port->as_int64());
  }

  if (const auto* policy = report.if_contains("policy"); policy != nullptr && policy->is_object()) node.policy = policy->as_object();

  if (const auto* discovered = report.if_contains("discovered"); discovered != nullptr && discovered->is_object()) {
    node.discovered = string_of(discovered->as_object(), "address");
  }

  if (const auto* reaches = report.if_contains("reaches"); reaches != nullptr && reaches->is_array()) {
    for (const auto& entry : reaches->as_array()) {
      if (!entry.is_object()) continue;
      const auto peer = string_of(entry.as_object(), "node");
      const auto address = string_of(entry.as_object(), "address");
      if (!peer.empty() && !address.empty()) node.reaches.emplace_back(peer, address);
    }
  }

  if (const auto* probes = report.if_contains("cluster_probes"); probes != nullptr && probes->is_array()) {
    for (const auto& entry : probes->as_array()) {
      if (!entry.is_object()) continue;
      const auto peer = string_of(entry.as_object(), "node");
      const auto* reached = entry.as_object().if_contains("reached");
      if (!peer.empty() && reached != nullptr && reached->is_bool()) node.cluster_probes.emplace_back(peer, reached->as_bool());
    }
  }

  std::lock_guard<std::mutex> lock(_mutex);
  _nodes[id] = std::move(node);
  return true;
}

namespace {

// seconds::max() is "never stale". Compared as it stands it would be converted to the clock's nanoseconds, which
// overflows.
bool is_stale(std::chrono::steady_clock::duration silent, std::chrono::seconds stale_after) {
  if (stale_after == std::chrono::seconds::max()) return false;
  return silent > std::chrono::duration_cast<std::chrono::steady_clock::duration>(stale_after);
}

}  // namespace

std::optional<NodeDirectory::Node> NodeDirectory::find(const std::string& id, std::chrono::seconds stale_after,
                                                       std::chrono::steady_clock::time_point now) const {
  std::lock_guard<std::mutex> lock(_mutex);

  const auto found = _nodes.find(id);
  if (found == _nodes.end()) return std::nullopt;

  auto node = found->second;
  node.stale = is_stale(now - node.heard, stale_after);
  return node;
}

std::vector<NodeDirectory::Node> NodeDirectory::list(std::chrono::seconds stale_after, std::chrono::steady_clock::time_point now) const {
  std::lock_guard<std::mutex> lock(_mutex);

  std::vector<Node> out;
  out.reserve(_nodes.size());

  for (const auto& [id, node] : _nodes) {
    out.push_back(node);
    out.back().stale = is_stale(now - node.heard, stale_after);
  }

  return out;
}

}  // namespace athenasip
