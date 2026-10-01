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

// The node a status topic names, or empty when the topic is not one.
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

  std::lock_guard<std::mutex> lock(_mutex);
  _nodes[id] = std::move(node);
  return true;
}

std::vector<NodeDirectory::Node> NodeDirectory::list(std::chrono::seconds stale_after, std::chrono::steady_clock::time_point now) const {
  std::lock_guard<std::mutex> lock(_mutex);

  std::vector<Node> out;
  out.reserve(_nodes.size());

  for (const auto& [id, node] : _nodes) {
    out.push_back(node);
    out.back().stale = now - node.heard > stale_after;
  }

  return out;
}

}  // namespace athenasip
