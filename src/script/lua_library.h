//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <lua.hpp>
#include <memory>
#include <vector>

#include "../policy/policy.h"
#include "../sip_message.h"
#include "../types/location.h"
#include "../types/realm.h"
#include "../types/sip_uri.h"
#include "../types/subscriber.h"

namespace athenasip::script {

// The athenasip global: log, node, config, store and sip, and the objects they hand out. Opened into each state
// before the prelude. docs/scripting.md is the reference.
void open_library(lua_State* L);

// What a hook is handed. Each refuses use once `live` is false, so nothing outlives the call it belongs to.
void push_request(lua_State* L, std::shared_ptr<policy::RequestView> request, std::shared_ptr<bool> live);
void push_response(lua_State* L, std::shared_ptr<SIPMessage> response, std::shared_ptr<bool> live);
void push_uri(lua_State* L, std::shared_ptr<types::SIPUri> uri);

// What a hook hands back. Null when the value at idx is not one.
std::shared_ptr<types::Realm> to_realm(lua_State* L, int idx);
std::shared_ptr<types::Subscriber> to_subscriber(lua_State* L, int idx);
std::shared_ptr<std::vector<types::Location>> to_bindings(lua_State* L, int idx);

// A URI object, or the text of one. Null for anything else, or text that does not parse.
std::shared_ptr<types::SIPUri> to_uri(lua_State* L, int idx);

}  // namespace athenasip::script
