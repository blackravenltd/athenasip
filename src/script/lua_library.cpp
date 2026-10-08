//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "lua_library.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <utility>

#include "../channel.h"
#include "../config.h"
#include "../config_schema.h"
#include "../headers/sip_identity_header.h"
#include "../loggers/logger.h"
#include "../types/trunk.h"
#include "../util.h"
#include "lua_engine.h"

namespace athenasip::script {

namespace {

constexpr char kRequest[] = "athenasip.request";
constexpr char kResponse[] = "athenasip.response";
constexpr char kUri[] = "athenasip.uri";
constexpr char kRealm[] = "athenasip.realm";
constexpr char kSubscriber[] = "athenasip.subscriber";
constexpr char kBindings[] = "athenasip.bindings";
constexpr char kTrunk[] = "athenasip.trunk";

// A node object inside a Lua userdata. live, when set, is the hook call's: a request kept in a global past its call
// refuses use rather than describe a message long gone.
template <typename T>
struct Box {
  std::shared_ptr<T> value;
  std::shared_ptr<bool> live;
};

template <typename T>
void push_box(lua_State* L, std::shared_ptr<T> value, std::shared_ptr<bool> live, const char* type) {
  void* memory = lua_newuserdatauv(L, sizeof(Box<T>), 0);
  new (memory) Box<T>{std::move(value), std::move(live)};
  luaL_setmetatable(L, type);
}

template <typename T>
Box<T>* check_box(lua_State* L, int idx, const char* type) {
  auto* box = static_cast<Box<T>*>(luaL_checkudata(L, idx, type));
  if (box->live && !*box->live) luaL_error(L, "%s from a hook call that has ended", type);
  return box;
}

template <typename T>
Box<T>* test_box(lua_State* L, int idx, const char* type) {
  return static_cast<Box<T>*>(luaL_testudata(L, idx, type));
}

template <typename T>
int collect(lua_State* L) {
  // __gc runs on an object of the right metatable only.
  static_cast<Box<T>*>(lua_touserdata(L, 1))->~Box<T>();
  return 0;
}

void push_text(lua_State* L, const std::string& value) { lua_pushlstring(L, value.data(), value.size()); }

// Nil for an empty string, which Lua scripts test more naturally than "".
void push_text_or_nil(lua_State* L, const std::string& value) {
  if (value.empty()) {
    lua_pushnil(L);
  } else {
    push_text(L, value);
  }
}

void set_text(lua_State* L, const char* key, const std::string& value) {
  push_text(L, value);
  lua_setfield(L, -2, key);
}

void set_integer(lua_State* L, const char* key, lua_Integer value) {
  lua_pushinteger(L, value);
  lua_setfield(L, -2, key);
}

void set_boolean(lua_State* L, const char* key, bool value) {
  lua_pushboolean(L, value ? 1 : 0);
  lua_setfield(L, -2, key);
}

void make_metatable(lua_State* L, const char* type, lua_CFunction index, lua_CFunction gc, std::initializer_list<luaL_Reg> more = {}) {
  luaL_newmetatable(L, type);
  lua_pushcfunction(L, index);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, gc);
  lua_setfield(L, -2, "__gc");
  for (const auto& entry : more) {
    lua_pushcfunction(L, entry.func);
    lua_setfield(L, -2, entry.name);
  }
  lua_pushstring(L, type);
  lua_setfield(L, -2, "__name");
  lua_pop(L, 1);
}

const Config& config_of(lua_State* L) {
  const auto* config = LuaEngine::of(L)->config();
  if (config == nullptr) luaL_error(L, "the configuration is not available yet");
  return *config;
}

// -- URIs

int uri_param(lua_State* L) {
  auto* box = check_box<types::SIPUri>(L, 1, kUri);
  const std::string name = luaL_checkstring(L, 2);
  if (!box->value->has_parameter(name)) return lua_pushnil(L), 1;
  push_text(L, box->value->parameter(name));
  return 1;
}

int uri_tostring(lua_State* L) {
  push_text(L, check_box<types::SIPUri>(L, 1, kUri)->value->to_string());
  return 1;
}

int uri_equal(lua_State* L) {
  auto* a = test_box<types::SIPUri>(L, 1, kUri);
  auto* b = test_box<types::SIPUri>(L, 2, kUri);
  lua_pushboolean(L, a && b && a->value->equivalent_to(*b->value));
  return 1;
}

int uri_index(lua_State* L) {
  const auto& uri = *check_box<types::SIPUri>(L, 1, kUri)->value;
  const std::string key = luaL_checkstring(L, 2);

  if (key == "scheme") return push_text(L, uri.scheme), 1;
  if (key == "user") return push_text_or_nil(L, uri.user), 1;
  if (key == "host") return push_text(L, uri.host), 1;
  if (key == "port") {
    if (uri.port) return lua_pushinteger(L, *uri.port), 1;
    return lua_pushnil(L), 1;
  }
  if (key == "transport") return push_text_or_nil(L, uri.has_parameter("transport") ? Util::to_lower(uri.parameter("transport")) : ""), 1;
  if (key == "param") return lua_pushcfunction(L, uri_param), 1;
  if (key == "tostring") return lua_pushcfunction(L, uri_tostring), 1;
  return lua_pushnil(L), 1;
}

// -- The request and the response

std::shared_ptr<types::SIPIdentity> identity_of(const SIPMessage& message, const std::string& field) {
  if (!message.header->contains(field)) return nullptr;

  auto header = message.header->headers_map.at(field)[0]->as<headers::SIPIdentityHeader>();
  if (header == nullptr || header->value == nullptr || header->value->uri == nullptr) return nullptr;
  return header->value;
}

// An address header as {uri =, display =, tag =}, or nil.
void push_address(lua_State* L, const SIPMessage& message, const std::string& field) {
  auto identity = identity_of(message, field);
  if (!identity) return lua_pushnil(L);

  lua_createtable(L, 0, 3);
  push_uri(L, identity->uri);
  lua_setfield(L, -2, "uri");
  if (identity->display_name) set_text(L, "display", *identity->display_name);
  if (const auto tag = identity->tags.find("tag"); tag != identity->tags.end()) set_text(L, "tag", tag->second);
}

int message_header(lua_State* L, const SIPMessage& message) {
  const std::string name = luaL_checkstring(L, 2);
  if (!message.header->contains(name)) return lua_pushnil(L), 1;
  push_text(L, message.header->headers_map.at(name)[0]->to_string());
  return 1;
}

int message_headers(lua_State* L, const SIPMessage& message) {
  const std::string name = luaL_checkstring(L, 2);
  lua_newtable(L);
  if (!message.header->contains(name)) return 1;

  lua_Integer i = 0;
  for (const auto& value : message.header->headers_map.at(name)) {
    push_text(L, value->to_string());
    lua_rawseti(L, -2, ++i);
  }
  return 1;
}

int request_header(lua_State* L) { return message_header(L, *check_box<policy::RequestView>(L, 1, kRequest)->value->message); }
int request_headers(lua_State* L) { return message_headers(L, *check_box<policy::RequestView>(L, 1, kRequest)->value->message); }

void push_source(lua_State* L, const SIPMessage& message) {
  auto channel = message.channel.lock();
  if (!channel || !channel->_connection) return lua_pushnil(L);

  const auto& connection = channel->_connection;
  const auto remote = connection->remote_endpoint();

  lua_createtable(L, 0, 6);
  set_text(L, "transport", Util::to_lower(connection->transport_name()));
  set_text(L, "address", remote.address().to_string());
  set_integer(L, "port", remote.port());
  set_boolean(L, "reliable", connection->is_reliable());
  set_boolean(L, "authenticated", channel->is_authenticated());
  set_text(L, "flow", channel->flow_id());
}

int request_index(lua_State* L) {
  const auto& view = *check_box<policy::RequestView>(L, 1, kRequest)->value;
  const auto& message = *view.message;
  const std::string key = luaL_checkstring(L, 2);

  if (key == "method") return push_text(L, message.header->request_method), 1;
  if (key == "uri") {
    if (!message.header->request_uri) return lua_pushnil(L), 1;
    return push_uri(L, message.header->request_uri), 1;
  }
  if (key == "from") return push_address(L, message, "From"), 1;
  if (key == "to") return push_address(L, message, "To"), 1;
  if (key == "call_id") return push_text_or_nil(L, message.header->contains("Call-ID") ? message.header->headers_map.at("Call-ID")[0]->to_string() : ""), 1;

  // RFC 3261 12: an in-dialog request is one with a To tag inside a dialog this node knows. A tag alone proves
  // nothing.
  if (key == "in_dialog") {
    auto to = identity_of(message, "To");
    return lua_pushboolean(L, to && to->tags.count("tag") != 0 && message.in_known_dialog), 1;
  }
  if (key == "from_peer") return lua_pushboolean(L, view.from_peer), 1;
  if (key == "has_flow_token") return lua_pushboolean(L, view.valid_flow_token), 1;
  if (key == "has_route") return lua_pushboolean(L, view.has_route), 1;
  if (key == "relay") return lua_pushboolean(L, view.relay), 1;
  if (key == "source") return push_source(L, message), 1;
  if (key == "header") return lua_pushcfunction(L, request_header), 1;
  if (key == "headers") return lua_pushcfunction(L, request_headers), 1;
  return lua_pushnil(L), 1;
}

int response_header(lua_State* L) { return message_header(L, *check_box<SIPMessage>(L, 1, kResponse)->value); }
int response_headers(lua_State* L) { return message_headers(L, *check_box<SIPMessage>(L, 1, kResponse)->value); }

int response_index(lua_State* L) {
  const auto& message = *check_box<SIPMessage>(L, 1, kResponse)->value;
  const std::string key = luaL_checkstring(L, 2);

  if (key == "code") return lua_pushinteger(L, message.header->response_code), 1;
  if (key == "reason") return push_text(L, message.header->response_message), 1;
  if (key == "header") return lua_pushcfunction(L, response_header), 1;
  if (key == "headers") return lua_pushcfunction(L, response_headers), 1;
  return lua_pushnil(L), 1;
}

// -- Realms, subscribers and bindings

void push_behaviour(lua_State* L, const types::MediaPolicy& media, std::uint32_t qualify, bool rewrite_contact) {
  lua_createtable(L, 0, 4);
  set_boolean(L, "media_anchor", media.anchor);
  set_text(L, "media_profile", types::MediaPolicy::to_string(media.profiles));
  set_integer(L, "qualify_interval", qualify);
  set_boolean(L, "rewrite_contact", rewrite_contact);
}

int realm_index(lua_State* L) {
  const auto& realm = *check_box<types::Realm>(L, 1, kRealm)->value;
  const std::string key = luaL_checkstring(L, 2);

  if (key == "name") return push_text(L, realm.name), 1;
  if (key == "id") return lua_pushinteger(L, static_cast<lua_Integer>(realm.id)), 1;
  if (key == "registration_timeout") return lua_pushinteger(L, realm.registration_timeout), 1;
  if (key == "registration_minimum") return lua_pushinteger(L, realm.registration_minimum), 1;

  // What the realm set itself, nil for what it inherits.
  if (key == "behaviour") {
    const auto& set = realm.behaviour;
    lua_createtable(L, 0, 4);
    if (set.media_anchor) set_boolean(L, "media_anchor", *set.media_anchor);
    if (set.media_profile) set_text(L, "media_profile", types::MediaPolicy::to_string(*set.media_profile));
    if (set.qualify_interval) set_integer(L, "qualify_interval", *set.qualify_interval);
    if (set.rewrite_contact) set_boolean(L, "rewrite_contact", *set.rewrite_contact);
    return 1;
  }

  // The realm over the server's defaults.
  if (key == "behaviour_effective") {
    const auto& config = config_of(L);
    push_behaviour(L, realm.behaviour.over(config.behaviour), realm.behaviour.qualify_over(config.behaviour_qualify_interval),
                   realm.behaviour.rewrite_contact.value_or(config.behaviour_rewrite_contact));
    return 1;
  }
  return lua_pushnil(L), 1;
}

int subscriber_index(lua_State* L) {
  const auto& subscriber = *check_box<types::Subscriber>(L, 1, kSubscriber)->value;
  const std::string key = luaL_checkstring(L, 2);
  const auto uri = subscriber.identity ? subscriber.identity->uri : nullptr;

  if (key == "id") return lua_pushinteger(L, static_cast<lua_Integer>(subscriber.id)), 1;
  if (key == "aor") return push_text(L, uri ? uri->to_string() : ""), 1;
  if (key == "uri") {
    if (!uri) return lua_pushnil(L), 1;
    return push_uri(L, uri), 1;
  }
  if (key == "realm") return push_text(L, uri ? Util::to_lower(uri->host) : ""), 1;
  if (key == "media_profile") {
    if (!subscriber.media_profile) return lua_pushnil(L), 1;
    return push_text(L, types::MediaPolicy::to_string(*subscriber.media_profile)), 1;
  }
  return lua_pushnil(L), 1;
}

int bindings_length(lua_State* L) {
  lua_pushinteger(L, static_cast<lua_Integer>(check_box<std::vector<types::Location>>(L, 1, kBindings)->value->size()));
  return 1;
}

int bindings_index(lua_State* L) {
  const auto& bindings = *check_box<std::vector<types::Location>>(L, 1, kBindings)->value;
  if (!lua_isinteger(L, 2)) return lua_pushnil(L), 1;

  const auto i = lua_tointeger(L, 2);
  if (i < 1 || static_cast<std::size_t>(i) > bindings.size()) return lua_pushnil(L), 1;

  const auto& binding = bindings[static_cast<std::size_t>(i - 1)];
  lua_createtable(L, 0, 8);
  if (binding.contact) set_text(L, "contact", binding.contact->to_string());
  set_text(L, "node_id", binding.node_id);
  set_text(L, "flow_id", binding.flow_id);
  set_integer(L, "registered_at", binding.registered_at);
  set_integer(L, "expires_at", binding.expires_at);
  set_text(L, "instance", binding.instance);
  set_integer(L, "reg_id", binding.reg_id);
  set_boolean(L, "push", binding.push);
  return 1;
}

// JSON as Lua: objects and arrays as tables (arrays from 1), null as nil.
void push_json(lua_State* L, const boost::json::value& value) {
  switch (value.kind()) {
    case boost::json::kind::object: {
      const auto& object = value.as_object();
      lua_createtable(L, 0, static_cast<int>(object.size()));
      for (const auto& [key, member] : object) {
        push_json(L, member);
        lua_setfield(L, -2, std::string(key).c_str());
      }
      return;
    }
    case boost::json::kind::array: {
      const auto& array = value.as_array();
      lua_createtable(L, static_cast<int>(array.size()), 0);
      lua_Integer i = 0;
      for (const auto& member : array) {
        push_json(L, member);
        lua_rawseti(L, -2, ++i);
      }
      return;
    }
    case boost::json::kind::string:
      return push_text(L, std::string(value.as_string()));
    case boost::json::kind::int64:
      return lua_pushinteger(L, value.as_int64());
    case boost::json::kind::uint64:
      return lua_pushinteger(L, static_cast<lua_Integer>(value.as_uint64()));
    case boost::json::kind::double_:
      return lua_pushnumber(L, value.as_double());
    case boost::json::kind::bool_:
      return lua_pushboolean(L, value.as_bool());
    case boost::json::kind::null:
      return lua_pushnil(L);
  }
}

int trunk_admits(lua_State* L) {
  const auto& trunk = *check_box<types::Trunk>(L, 1, kTrunk)->value;
  lua_pushboolean(L, trunk.admits(luaL_checkstring(L, 2)));
  return 1;
}

// Everything but the password, which is the node's to answer challenges with and no script's to read.
int trunk_index(lua_State* L) {
  const auto& trunk = *check_box<types::Trunk>(L, 1, kTrunk)->value;
  const std::string key = luaL_checkstring(L, 2);

  if (key == "name") return push_text(L, trunk.name), 1;
  if (key == "uri") return push_uri(L, std::make_shared<types::SIPUri>(trunk.uri)), 1;
  if (key == "username") return push_text_or_nil(L, trunk.username), 1;
  if (key == "registers") return lua_pushboolean(L, trunk.register_enabled), 1;
  if (key == "contact_user") return push_text_or_nil(L, trunk.contact_user), 1;
  if (key == "inbound_addresses") {
    lua_createtable(L, static_cast<int>(trunk.inbound_addresses.size()), 0);
    lua_Integer i = 0;
    for (const auto& range : trunk.inbound_addresses) {
      push_text(L, range);
      lua_rawseti(L, -2, ++i);
    }
    return 1;
  }
  if (key == "attributes") return push_json(L, trunk.attributes), 1;
  if (key == "admits") return lua_pushcfunction(L, trunk_admits), 1;
  return lua_pushnil(L), 1;
}

// -- athenasip.store: each lookup yields until the datastore answers.

policy::Host& host_of(lua_State* L) {
  const auto& host = LuaEngine::of(L)->host();
  if (!host) luaL_error(L, "the store is not available yet");
  return *host;
}

template <typename T>
std::function<int(lua_State*)> push_failure(const plugins::Result<T>& result) {
  return [error = result.error](lua_State* L) { return push_text(L, "the store failed - " + error), 1; };
}

int store_realm(lua_State* L) {
  std::string name = luaL_checkstring(L, 1);
  auto& host = host_of(L);
  return LuaEngine::await(L, [&host, name](LuaEngine::Answer answer) {
    host.realm(name, [answer](plugins::Result<std::shared_ptr<types::Realm>> found) {
      if (!found.ok) return answer(false, push_failure(found));
      answer(true, [realm = found.value](lua_State* L) {
        if (!realm) return lua_pushnil(L), 1;
        return push_box(L, realm, nullptr, kRealm), 1;
      });
    });
  });
}

int store_subscriber(lua_State* L) {
  auto uri = to_uri(L, 1);
  if (!uri) return luaL_argerror(L, 1, "an address of record, as a URI or its text");

  auto identity = std::make_shared<types::SIPIdentity>(uri->to_string());
  auto& host = host_of(L);
  return LuaEngine::await(L, [&host, identity](LuaEngine::Answer answer) {
    host.subscriber(identity, [answer](plugins::Result<std::shared_ptr<types::Subscriber>> found) {
      if (!found.ok) return answer(false, push_failure(found));
      answer(true, [subscriber = found.value](lua_State* L) {
        if (!subscriber) return lua_pushnil(L), 1;
        return push_box(L, subscriber, nullptr, kSubscriber), 1;
      });
    });
  });
}

int store_locations(lua_State* L) {
  const auto subscriber = check_box<types::Subscriber>(L, 1, kSubscriber)->value;
  auto& host = host_of(L);
  return LuaEngine::await(L, [&host, id = subscriber->id](LuaEngine::Answer answer) {
    host.locations(id, [answer](plugins::Result<std::vector<types::Location>> found) {
      if (!found.ok) return answer(false, push_failure(found));
      answer(true, [bindings = std::make_shared<std::vector<types::Location>>(std::move(found.value))](lua_State* L) {
        return push_box(L, bindings, nullptr, kBindings), 1;
      });
    });
  });
}

int store_trunk(lua_State* L) {
  std::string name = luaL_checkstring(L, 1);
  auto& host = host_of(L);
  return LuaEngine::await(L, [&host, name](LuaEngine::Answer answer) {
    host.trunk(name, [answer](plugins::Result<std::shared_ptr<types::Trunk>> found) {
      if (!found.ok) return answer(false, push_failure(found));
      answer(true, [trunk = found.value](lua_State* L) {
        if (!trunk) return lua_pushnil(L), 1;
        return push_box(L, trunk, nullptr, kTrunk), 1;
      });
    });
  });
}

// Every trunk, in name order.
int store_trunks(lua_State* L) {
  auto& host = host_of(L);
  return LuaEngine::await(L, [&host](LuaEngine::Answer answer) {
    host.trunks([answer](plugins::Result<std::vector<std::shared_ptr<types::Trunk>>> found) {
      if (!found.ok) return answer(false, push_failure(found));

      auto trunks = std::move(found.value);
      std::sort(trunks.begin(), trunks.end(), [](const auto& a, const auto& b) { return a->key() < b->key(); });
      answer(true, [trunks = std::move(trunks)](lua_State* L) {
        lua_createtable(L, static_cast<int>(trunks.size()), 0);
        lua_Integer i = 0;
        for (const auto& trunk : trunks) {
          push_box(L, trunk, nullptr, kTrunk);
          lua_rawseti(L, -2, ++i);
        }
        return 1;
      });
    });
  });
}

// -- athenasip.node and athenasip.config, read when asked: the host is attached after the scripts load.

int node_names(lua_State* L) {
  const std::string host = luaL_checkstring(L, 1);
  const auto port = static_cast<std::uint16_t>(luaL_optinteger(L, 2, 5060));
  lua_pushboolean(L, host_of(L).names_this_node(host, port));
  return 1;
}

int node_index(lua_State* L) {
  const std::string key = luaL_checkstring(L, 2);
  const auto& config = config_of(L);

  if (key == "id") return push_text(L, config.sip_node_id), 1;
  if (key == "public_address") return push_text_or_nil(L, config.sip_public_address), 1;
  return lua_pushnil(L), 1;
}

// A setting by its dotted name: what the node parsed where that differs from the file's text, else the file's
// value, else the default. Nil for a name that is no setting.
int config_get(lua_State* L) {
  const std::string key = luaL_checkstring(L, 1);
  const auto& config = config_of(L);

  if (key == "sip.node_id") return push_text(L, config.sip_node_id), 1;
  if (key == "sip.public_address") return push_text_or_nil(L, config.sip_public_address), 1;
  if (key == "sip.forward_register") return push_text(L, config.sip_forward_register), 1;
  if (key == "behaviour.media_anchor") return lua_pushboolean(L, config.behaviour.anchor), 1;
  if (key == "behaviour.media_profile") return push_text(L, types::MediaPolicy::to_string(config.behaviour.profiles)), 1;
  if (key == "behaviour.qualify_interval") return lua_pushinteger(L, config.behaviour_qualify_interval), 1;
  if (key == "behaviour.rewrite_contact") return lua_pushboolean(L, config.behaviour_rewrite_contact), 1;

  static const auto settings = all_config_settings();
  const plugins::Setting* setting = nullptr;
  for (const auto& candidate : settings) {
    if (candidate.key == key) setting = &candidate;
  }

  // Assignment to a YAML::Node writes through to the document; reset() rebinds.
  YAML::Node node;
  node.reset(config.root());
  std::size_t start = 0;
  while (node && node.IsMap() && start <= key.size()) {
    const auto dot = key.find('.', start);
    const auto part = key.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    YAML::Node next = node[part];
    node.reset(next);
    if (dot == std::string::npos) break;
    start = dot + 1;
  }

  std::string text;
  if (node && node.IsScalar()) {
    text = node.Scalar();
  } else if (setting && setting->type != plugins::Setting::Type::Section && setting->type != plugins::Setting::Type::List) {
    text = setting->fallback;
  } else {
    return lua_pushnil(L), 1;
  }

  const auto type = setting ? setting->type : plugins::Setting::Type::String;
  if (type == plugins::Setting::Type::Boolean) return lua_pushboolean(L, Util::to_lower(text) == "true"), 1;
  if (type == plugins::Setting::Type::Integer) {
    try {
      return lua_pushinteger(L, std::stoll(text)), 1;
    } catch (const std::exception&) {
      return lua_pushnil(L), 1;
    }
  }
  return push_text(L, text), 1;
}

int config_index(lua_State* L) {
  const std::string key = luaL_checkstring(L, 2);
  if (key != "behaviour") return lua_pushnil(L), 1;

  const auto& config = config_of(L);
  push_behaviour(L, config.behaviour, config.behaviour_qualify_interval, config.behaviour_rewrite_contact);
  return 1;
}

// -- athenasip.log

template <int Level>
int log(lua_State* L) {
  const auto logger = LuaEngine::of(L)->script_logger();

  // Where the script called from, so a line in the log leads back to the line in the script.
  lua_Debug where{};
  std::string prefix;
  if (lua_getstack(L, 1, &where) && lua_getinfo(L, "Sl", &where) && where.currentline > 0) {
    prefix = std::string(where.short_src) + ":" + std::to_string(where.currentline) + ": ";
  }

  const std::string message = prefix + luaL_tolstring(L, 1, nullptr);
  lua_pop(L, 1);

  if constexpr (Level == 0) logger->debug(message);
  if constexpr (Level == 1) logger->info(message);
  if constexpr (Level == 2) logger->warn(message);
  if constexpr (Level == 3) logger->error(message);
  return 0;
}

// -- athenasip.sip

// Whether an address is in a CIDR range, or in any of a list of them.
int sip_in_range(lua_State* L) {
  const std::string address = luaL_checkstring(L, 2);
  if (lua_type(L, 1) == LUA_TSTRING) return lua_pushboolean(L, types::in_range(lua_tostring(L, 1), address)), 1;

  luaL_checktype(L, 1, LUA_TTABLE);
  const auto count = luaL_len(L, 1);
  for (lua_Integer i = 1; i <= count; ++i) {
    lua_geti(L, 1, i);
    const bool inside = lua_type(L, -1) == LUA_TSTRING && types::in_range(lua_tostring(L, -1), address);
    lua_pop(L, 1);
    if (inside) return lua_pushboolean(L, 1), 1;
  }
  return lua_pushboolean(L, 0), 1;
}

int sip_uri(lua_State* L) {
  auto uri = to_uri(L, 1);
  if (!uri) return luaL_argerror(L, 1, "the text of a SIP URI");
  push_uri(L, uri);
  return 1;
}

int is_realm(lua_State* L) { return lua_pushboolean(L, test_box<types::Realm>(L, 1, kRealm) != nullptr), 1; }
int is_subscriber(lua_State* L) { return lua_pushboolean(L, test_box<types::Subscriber>(L, 1, kSubscriber) != nullptr), 1; }

void set_functions(lua_State* L, std::initializer_list<luaL_Reg> functions) {
  for (const auto& entry : functions) {
    lua_pushcfunction(L, entry.func);
    lua_setfield(L, -2, entry.name);
  }
}

// A table whose fields are read when asked, from index.
void set_lazy_table(lua_State* L, const char* name, lua_CFunction index, std::initializer_list<luaL_Reg> functions) {
  lua_newtable(L);
  set_functions(L, functions);
  lua_newtable(L);
  lua_pushcfunction(L, index);
  lua_setfield(L, -2, "__index");
  lua_setmetatable(L, -2);
  lua_setfield(L, -2, name);
}

}  // namespace

void push_request(lua_State* L, std::shared_ptr<policy::RequestView> request, std::shared_ptr<bool> live) {
  push_box(L, std::move(request), std::move(live), kRequest);
}

void push_response(lua_State* L, std::shared_ptr<SIPMessage> response, std::shared_ptr<bool> live) {
  push_box(L, std::move(response), std::move(live), kResponse);
}

void push_uri(lua_State* L, std::shared_ptr<types::SIPUri> uri) { push_box(L, std::move(uri), nullptr, kUri); }

std::shared_ptr<types::Realm> to_realm(lua_State* L, int idx) {
  auto* box = test_box<types::Realm>(L, idx, kRealm);
  return box ? box->value : nullptr;
}

std::shared_ptr<types::Subscriber> to_subscriber(lua_State* L, int idx) {
  auto* box = test_box<types::Subscriber>(L, idx, kSubscriber);
  return box ? box->value : nullptr;
}

std::shared_ptr<std::vector<types::Location>> to_bindings(lua_State* L, int idx) {
  auto* box = test_box<std::vector<types::Location>>(L, idx, kBindings);
  return box ? box->value : nullptr;
}

std::shared_ptr<types::SIPUri> to_uri(lua_State* L, int idx) {
  if (auto* box = test_box<types::SIPUri>(L, idx, kUri)) return box->value;
  if (lua_type(L, idx) != LUA_TSTRING) return nullptr;

  auto uri = std::make_shared<types::SIPUri>(lua_tostring(L, idx));
  return uri->valid ? uri : nullptr;
}

void open_library(lua_State* L) {
  make_metatable(L, kRequest, request_index, collect<policy::RequestView>);
  make_metatable(L, kResponse, response_index, collect<SIPMessage>);
  make_metatable(L, kUri, uri_index, collect<types::SIPUri>, {{"__tostring", uri_tostring}, {"__eq", uri_equal}});
  make_metatable(L, kRealm, realm_index, collect<types::Realm>);
  make_metatable(L, kSubscriber, subscriber_index, collect<types::Subscriber>);
  make_metatable(L, kBindings, bindings_index, collect<std::vector<types::Location>>, {{"__len", bindings_length}});
  make_metatable(L, kTrunk, trunk_index, collect<types::Trunk>);

  lua_newtable(L);

  set_functions(L, {{"is_realm", is_realm}, {"is_subscriber", is_subscriber}});

  lua_newtable(L);
  set_functions(L, {{"debug", log<0>}, {"info", log<1>}, {"warn", log<2>}, {"error", log<3>}});
  lua_setfield(L, -2, "log");

  lua_newtable(L);
  set_functions(L,
                {{"realm", store_realm}, {"subscriber", store_subscriber}, {"locations", store_locations}, {"trunk", store_trunk}, {"trunks", store_trunks}});
  lua_setfield(L, -2, "store");

  lua_newtable(L);
  set_functions(L, {{"uri", sip_uri}, {"in_range", sip_in_range}});
  lua_setfield(L, -2, "sip");

  set_lazy_table(L, "node", node_index, {{"names", node_names}});
  set_lazy_table(L, "config", config_index, {{"get", config_get}});

  lua_setglobal(L, "athenasip");

  // print goes to the log, not to the node's standard output.
  lua_pushcfunction(L, log<1>);
  lua_setglobal(L, "print");
}

}  // namespace athenasip::script
