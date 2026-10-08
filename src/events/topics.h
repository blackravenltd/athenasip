//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

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

inline std::string call_register(const std::string& call_id) { return "calls/" + call_id + "/register"; }

inline std::string call_unregister(const std::string& call_id) { return "calls/" + call_id + "/unregister"; }

// Every state a call passes through (the RFC 3261 section 12 dialog), so a UI can follow
// a call without polling.
inline std::string call_state(const std::string& call_id) { return "calls/" + call_id + "/state"; }

}  // namespace athenasip::events::topics
