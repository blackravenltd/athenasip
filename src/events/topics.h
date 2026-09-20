//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <string>

// The event topic scheme, defined once. Every topic AthenaSIP publishes is built here
// so the shape cannot drift between call sites. Topics are hierarchical, use '/' as the
// only separator, and never begin with '/': a leading slash is a distinct, empty first
// level in MQTT, so "nodes/x" and "/nodes/x" are different topics and a "nodes/#"
// filter does not match the second.
//
// The scheme is documented in docs/events.md.
namespace athenasip::events::topics {

inline std::string node_status(const std::string& node_id) { return "nodes/" + node_id + "/status"; }

inline std::string node_channel(const std::string& node_id, const std::string& transport, const std::string& endpoint) {
  return "nodes/" + node_id + "/channels/" + transport + "/" + endpoint;
}

inline std::string node_transaction(const std::string& node_id, const std::string& transaction_id) {
  return "nodes/" + node_id + "/transactions/" + transaction_id;
}

inline std::string subscriber_status(const std::string& uri) { return "subscriber/" + uri + "/status"; }

inline std::string call_register(const std::string& call_id) { return "calls/" + call_id + "/register"; }

inline std::string call_unregister(const std::string& call_id) { return "calls/" + call_id + "/unregister"; }

// Every state a call passes through, from the dialog it hangs off (RFC 3261 section 12).
// This is what an admin UI watches to show a call ringing and then connected without
// polling for it.
inline std::string call_state(const std::string& call_id) { return "calls/" + call_id + "/state"; }

}  // namespace athenasip::events::topics
