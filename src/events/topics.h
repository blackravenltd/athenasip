//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cctype>
#include <string>

// The event topic scheme (docs/events.md), built here so call sites cannot drift. Topics
// are '/'-separated and never begin with '/': in MQTT a leading slash is an empty first
// level, so a "nodes/#" filter would not match "/nodes/x".
namespace athenasip::events::topics {

inline std::string node_status(const std::string& node_id) { return "nodes/" + node_id + "/status"; }

inline std::string node_channel(const std::string& node_id, const std::string& transport, const std::string& endpoint) {
  return "nodes/" + node_id + "/channels/" + transport + "/" + endpoint;
}

inline std::string node_transaction(const std::string& node_id, const std::string& transaction_id) {
  return "nodes/" + node_id + "/transactions/" + transaction_id;
}

inline std::string subscriber_status(const std::string& uri) { return "subscribers/" + uri + "/status"; }

// Retained: whether the node holding a trunk's registration has it registered.
inline std::string trunk_status(const std::string& name) { return "trunks/" + name + "/status"; }

// A trunk created, changed or deleted over the API, so every node reads the trunks now rather than at its next
// scan. A signal, not state: a node that misses it reads them at the scan anyway.
inline std::string trunks_changed() { return "trunks/changed"; }

// A Digest answer over SIP that did not verify, and an address banned for too many (AuthGuard). Signals, for a
// console or a fail2ban-like watcher; not retained.
inline std::string auth_failed() { return "security/auth_failed"; }
inline std::string source_banned() { return "security/banned"; }
// An AOR closed to strangers for too many failures for it; an administrator lifting a ban or reopening an AOR. Every
// node holds what the others ban and lifts what any lifts, so the list is the cluster's.
inline std::string aor_closed() { return "security/aor_closed"; }
inline std::string ban_lifted() { return "security/ban_lifted"; }
inline std::string aor_reopened() { return "security/aor_reopened"; }

inline std::string call_register(const std::string& call_id) { return "calls/" + call_id + "/register"; }

inline std::string call_unregister(const std::string& call_id) { return "calls/" + call_id + "/unregister"; }

// Every state a call passes through (the RFC 3261 section 12 dialog), so a UI can follow
// a call without polling.
inline std::string call_state(const std::string& call_id) { return "calls/" + call_id + "/state"; }

// A presentity's state changed: look again (docs/design/presence.md). A signal, never the state. The AOR is
// percent-encoded, because a subscriber may be named "+4420..." and MQTT allows neither + nor # in a topic name, nor
// / within a level.
inline std::string presence_changed(const std::string& aor) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string level;
  for (const unsigned char c : aor) {
    if (std::isalnum(c) || c == ':' || c == '@' || c == '.' || c == '-' || c == '_' || c == '~') {
      level += static_cast<char>(c);
    } else {
      level += '%';
      level += kHex[c >> 4];
      level += kHex[c & 0x0F];
    }
  }
  return "presence/" + level + "/changed";
}

inline const char* presence_changed_filter() { return "presence/+/changed"; }

// The AOR a presence_changed topic names, or empty for any other topic.
inline std::string presence_changed_aor(const std::string& topic) {
  static const std::string prefix = "presence/";
  static const std::string suffix = "/changed";
  if (topic.size() <= prefix.size() + suffix.size() || topic.compare(0, prefix.size(), prefix) != 0 ||
      topic.compare(topic.size() - suffix.size(), suffix.size(), suffix) != 0) {
    return "";
  }

  const auto level = topic.substr(prefix.size(), topic.size() - prefix.size() - suffix.size());
  if (level.find('/') != std::string::npos) return "";

  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };

  std::string aor;
  for (std::size_t i = 0; i < level.size(); ++i) {
    if (level[i] == '%' && i + 2 < level.size() && hex(level[i + 1]) >= 0 && hex(level[i + 2]) >= 0) {
      aor += static_cast<char>(hex(level[i + 1]) * 16 + hex(level[i + 2]));
      i += 2;
    } else {
      aor += level[i];
    }
  }
  return aor;
}

}  // namespace athenasip::events::topics
