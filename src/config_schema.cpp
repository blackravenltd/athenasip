//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "config_schema.h"

#include <algorithm>
#include <boost/json.hpp>
#include <map>
#include <sstream>

namespace athenasip {

using plugins::Setting;
using plugins::Settings;

namespace {

Setting section(std::string key, std::string description, bool open = false) {
  Setting setting;
  setting.key = std::move(key);
  setting.type = Setting::Type::Section;
  setting.description = std::move(description);
  setting.open = open;
  return setting;
}

Setting boolean(std::string key, std::string fallback, std::string description) {
  Setting setting;
  setting.key = std::move(key);
  setting.type = Setting::Type::Boolean;
  setting.fallback = std::move(fallback);
  setting.description = std::move(description);
  return setting;
}

Setting integer(std::string key, std::string fallback, std::string description, std::optional<std::int64_t> minimum = std::nullopt,
                std::optional<std::int64_t> maximum = std::nullopt, bool or_zero = false) {
  Setting setting;
  setting.key = std::move(key);
  setting.type = Setting::Type::Integer;
  setting.fallback = std::move(fallback);
  setting.description = std::move(description);
  setting.minimum = minimum;
  setting.maximum = maximum;
  setting.or_zero = or_zero;
  return setting;
}

Setting port(std::string key, std::string fallback, std::string description) {
  return integer(std::move(key), std::move(fallback), std::move(description), 0, 65535);
}

Setting text(std::string key, std::string fallback, std::string description) {
  Setting setting;
  setting.key = std::move(key);
  setting.type = Setting::Type::String;
  setting.fallback = std::move(fallback);
  setting.description = std::move(description);
  return setting;
}

Setting list(std::string key, std::string description) {
  Setting setting;
  setting.key = std::move(key);
  setting.type = Setting::Type::List;
  setting.fallback = "[]";
  setting.description = std::move(description);
  return setting;
}

Setting choice(std::string key, std::string fallback, std::vector<std::string> choices, std::string description) {
  Setting setting;
  setting.key = std::move(key);
  setting.type = Setting::Type::Choice;
  setting.fallback = std::move(fallback);
  setting.choices = std::move(choices);
  setting.description = std::move(description);
  return setting;
}

Setting required(Setting setting) {
  setting.required = true;
  return setting;
}

// udp, tcp, tls and websocket share these.
void add_listener(Settings& settings, const std::string& name, const std::string& description) {
  settings.push_back(section(name, description));
  settings.push_back(boolean(name + ".enable", "true", "A listener whose section is present is enabled unless this is false."));
  settings.push_back(text(name + ".address", "0.0.0.0", "The address to bind."));
  settings.push_back(required(port(name + ".port", "", "The port to bind. Required unless enable is false.")));
  settings.push_back(port(name + ".public_port", "0", "The port a router forwards to this listener, for what the node tells others. 0 is the bound port."));
}

Settings build() {
  Settings s;

  s.push_back(required(section("sip", "The node, and how it treats SIP.")));
  s.push_back(required(text("sip.node_id", "", "This node's name. Unique in the cluster.")));
  s.push_back(text("sip.public_address", "",
                   "The address this node tells others to reach it on. Needed in a container, behind NAT or a balancer; empty falls back "
                   "to the bind address, or to one peers have seen this node at."));
  s.push_back(list("sip.localnet",
                   "Prefixes on this node's side of the router, such as 192.168.0.0/16. A far end inside them is given the local address "
                   "and port; everyone else the public ones."));
  s.push_back(boolean("sip.allow_unencrypted", "true", "False refuses to start with a udp, tcp or plain websocket listener."));
  s.push_back(choice("sip.forward_register", "subscribers", {"subscribers", "never"},
                     "A REGISTER for a domain this node does not serve (RFC 3261 10.3): subscribers forwards one from this node's own "
                     "subscribers; never answers 403."));
  s.push_back(boolean("sip.log_messages", "false", "Log every message in full, not only its first line. Credentials are redacted."));
  s.push_back(integer("sip.session_expires", "1800", "RFC 4028: the session interval, in seconds, put on a call that asked for none. 0 inserts none.", 90,
                      std::nullopt, true));
  s.push_back(integer("sip.session_min_se", "90", "RFC 4028: the shortest session interval a call may negotiate, in seconds.", 90));
  s.push_back(boolean("sip.require_session_timer", "false",
                      "Add Require: timer when the caller did not. NOT RECOMMENDED by RFC 4028: every call to an endpoint without the "
                      "extension fails."));
  s.push_back(
      integer("sip.max_call_duration", "0", "The longest any call is held, in seconds; each end is then sent a BYE. 0 is no limit. It cuts live calls too."));
  s.push_back(integer("sip.media_timeout", "300",
                      "Seconds an anchored call may carry no media before each end is sent a BYE. 0 turns it off. RTCP counts, so a call "
                      "on hold is not silent."));
  s.push_back(integer("sip.connect_timeout_ms", "4000", "How long opening a connection to a next hop may take, in milliseconds."));
  s.push_back(integer("sip.flow_idle_timeout", "300", "Seconds a UDP flow may sit idle before it is forgotten. 0 never forgets. Keep it above 32 (64 * T1)."));

  s.push_back(section("sip.timers", "The RFC 3261 section 17 timers. a to k are multiples of T1 or T4, not milliseconds. Leave them alone."));
  s.push_back(integer("sip.timers.t1_rtt_ms", "500", "T1, the round-trip estimate, in milliseconds.", 1, 65535));
  s.push_back(integer("sip.timers.t2_max_retransmit_interval_ms", "4000", "T2, the longest retransmit interval, in milliseconds.", 1, 65535));
  s.push_back(integer("sip.timers.t4_network_propagation_ms", "5000", "T4, the longest a message stays in the network, in milliseconds.", 1, 65535));
  s.push_back(integer("sip.timers.a_invite_initial", "1", "Timer A, the first INVITE retransmit, in T1.", 1, 65535));
  s.push_back(integer("sip.timers.b_invite_timeout", "64", "Timer B, the INVITE transaction timeout, in T1.", 1, 65535));
  s.push_back(integer("sip.timers.d_invite_duration", "64", "Timer D, the wait for response retransmits, in T1.", 0, 65535));
  s.push_back(integer("sip.timers.e_non_invite_initial", "1", "Timer E, the first non-INVITE retransmit, in T1.", 1, 65535));
  s.push_back(integer("sip.timers.f_non_invite_timeout", "64", "Timer F, the non-INVITE transaction timeout, in T1.", 1, 65535));
  s.push_back(integer("sip.timers.g_server_invite_initial", "1", "Timer G, the first INVITE response retransmit, in T1.", 1, 65535));
  s.push_back(integer("sip.timers.h_server_invite_timeout", "64", "Timer H, the wait for an ACK, in T1.", 1, 65535));
  s.push_back(integer("sip.timers.i_server_invite_duration", "1", "Timer I, the wait for ACK retransmits, in T4.", 0, 65535));
  s.push_back(integer("sip.timers.j_server_non_invite_duration", "64", "Timer J, the wait for non-INVITE retransmits, in T1.", 0, 65535));
  s.push_back(integer("sip.timers.k_non_invite_duration", "1", "Timer K, the wait for response retransmits, in T4.", 0, 65535));
  s.push_back(integer("sip.timers.c_invite_proxy_ms", "240000",
                      "Timer C, how long a proxied INVITE may ring, in milliseconds. Over three minutes (RFC 3261 16.6 step 11).", 180001));

  add_listener(s, "udp", "SIP over UDP. No section, no listener.");
  add_listener(s, "tcp", "SIP over TCP. No section, no listener.");
  add_listener(s, "tls", "SIP over TLS. No section, no listener.");
  s.push_back(text("tls.cert_pem_filename", "", "The certificate, PEM."));
  s.push_back(text("tls.key_pem_filename", "", "The certificate's key, PEM."));
  add_listener(s, "websocket", "SIP over WebSocket (RFC 7118), for browsers. No section, no listener.");
  s.push_back(boolean("websocket.tls", "false", "Serve wss on port. Needs this section's own certificate and key. A page served over https can only use wss."));
  s.push_back(port("websocket.secure_port", "0", "A second, wss listener beside a plain one. 0 is none. Unused when tls is true."));
  s.push_back(text("websocket.cert_pem_filename", "", "The wss certificate, PEM. Empty uses the tls section's."));
  s.push_back(text("websocket.key_pem_filename", "", "The wss certificate's key, PEM. Empty uses the tls section's."));

  s.push_back(section("cluster", "The inter-node listener: mutual TLS under the cluster CA (docs/certificates.md)."));
  s.push_back(boolean("cluster.enable", "false", "Join a cluster. Needs a shared datastore, an event bus, and ca, cert and key."));
  s.push_back(text("cluster.address", "0.0.0.0", "The address to bind."));
  s.push_back(port("cluster.port", "5062", "The port to bind."));
  s.push_back(text("cluster.advertise", "",
                   "What other nodes dial. The node's certificate must name it. Empty is the bind address, or sip.public_address on a "
                   "wildcard bind."));
  s.push_back(text("cluster.ca", "", "The cluster CA's certificate, from athenasip --ca-init."));
  s.push_back(text("cluster.cert", "", "This node's certificate, from athenasip --ca-node."));
  s.push_back(text("cluster.key", "", "This node's key."));

  s.push_back(section("datastore", "Where users, registrations and calls are kept. Each driver may have a section of its own here, named after it.", true));
  s.push_back(required(text("datastore.url", "memory://", "memory:// keeps nothing over a restart and serves one node; a cluster shares redis://.")));

  s.push_back(section("events", "The event bus: observability, presence and discovery. Each driver may have a section of its own here.", true));
  s.push_back(required(text("events.url", "local://", "local:// stays in this process; nodes find each other over mqtt://.")));
  s.push_back(integer("events.status_interval", "30", "Seconds between this node's status reports on the bus. 0 reports once."));

  s.push_back(section("media", "The media engine that relays calls. Each driver may have a section of its own here.", true));
  s.push_back(text("media.url", "builtin://", "builtin:// relays RTP; WebRTC (ICE, DTLS, SRTP) needs rtpengine://host:port."));

  s.push_back(section("plugins", "Plugin modules, loaded at start."));
  Setting plugin_path = list("plugins.path", "Directories of plugin modules (.so, .dylib, .dll).");
  plugin_path.or_single = true;
  s.push_back(plugin_path);

  s.push_back(section("push", "RFC 8599 push notifications. Off with no urls. Each driver may have a section of its own here.", true));
  s.push_back(list("push.urls", "One push service per entry: apns://, fcm://, webpush://."));
  s.push_back(integer("push.timeout", "10", "Seconds a request waits for the client to re-register after a push (RFC 8599 5.6.2).", 1, 30));
  s.push_back(integer("push.refresh", "180",
                      "Seconds before a push binding expires that a push asks the client to refresh it (RFC 8599 5.5). A push binding must "
                      "then ask for at least this plus 60.",
                      121));

  s.push_back(section("behaviour", "The defaults for what differs between SIP servers. A realm overrides any of them over the API."));
  s.push_back(boolean("behaviour.media_anchor", "true", "Relay media through the media engine. False leaves it end to end, which fails behind NAT."));
  s.push_back(choice("behaviour.media_profile", "mirror", {"mirror", "transport", "webrtc", "rtp", "srtp"},
                     "What each leg's media is offered as: mirror gives each leg what it offered; the others are explained in "
                     "docs/behaviour.md."));
  s.push_back(integer("behaviour.qualify_interval", "0", "Seconds between OPTIONS to a registered client. 0 is never.", 5, 86400, true));
  s.push_back(boolean("behaviour.rewrite_contact", "false", "Rewrite a registering client's Contact to the address it was seen at."));

  s.push_back(section("log", "What the node logs, and how."));
  s.push_back(choice("log.level", "debug", {"debug", "info", "warn", "error"}, "The least severe message logged."));
  s.push_back(choice("log.format", "text", {"text", "json"}, "One line of text per message, or one JSON object."));

  s.push_back(section("calls", "The record of calls."));
  s.push_back(integer("calls.history_retention", "2592000", "Seconds an ended call's record is kept. 0 keeps records for ever."));

  s.push_back(section("http", "The admin API, the console and the file server. No section, no listener."));
  s.push_back(text("http.address", "0.0.0.0", "The address to bind."));
  s.push_back(required(port("http.port", "", "The port to bind.")));

  s.push_back(section("http.tls", "An HTTPS listener beside the plain one. Browsers give the microphone only to a secure page."));
  s.push_back(boolean("http.tls.enable", "false", "Serve HTTPS."));
  s.push_back(text("http.tls.address", "", "The address to bind. Empty is http.address."));
  s.push_back(port("http.tls.port", "8443", "The port to bind."));
  s.push_back(text("http.tls.cert_pem_filename", "", "The certificate, PEM. Empty uses the tls section's."));
  s.push_back(text("http.tls.key_pem_filename", "", "The certificate's key, PEM. Empty uses the tls section's."));

  s.push_back(section("http.api", "The admin and client API (docs/api/openapi.yaml)."));
  s.push_back(boolean("http.api.enable", "true", "Serve the API when its section is present."));
  s.push_back(integer("http.api.session_lifetime", "43200", "An admin session's absolute lifetime, in seconds.", 1));
  s.push_back(integer("http.api.session_idle", "3600", "How long an admin session may sit unused, in seconds. 0 turns the check off."));
  Setting ice =
      list("http.api.ice_servers", "STUN and TURN URLs handed to web clients, each a URL or a map with url. They do not affect this node's own media.");
  ice.entry_key = "url";
  s.push_back(ice);
  s.push_back(text("http.api.turn_shared_secret", "",
                   "The secret shared with the TURN server (coturn's static-auth-secret), from which expiring credentials are minted. Empty "
                   "serves turn: URLs without credentials."));
  s.push_back(integer("http.api.turn_credential_ttl", "3600", "How long a minted TURN credential lasts, in seconds.", 1));

  s.push_back(section("http.api.rate_limits", "A burst allowance, then a rate per minute. 0 in either turns that limit off."));
  for (const auto& [name, what, burst, per_minute] : std::initializer_list<std::tuple<const char*, const char*, const char*, const char*>>{
           {"open", "Open routes, unknown endpoints and credentials that do not resolve, by source address.", "30", "30"},
           {"login_source", "Sign-ins, by source address.", "10", "5"},
           {"login_user", "Sign-ins, by username.", "5", "1"},
           {"session", "A signed-in caller, by session.", "60", "300"}}) {
    const std::string key = std::string("http.api.rate_limits.") + name;
    s.push_back(section(key, what));
    s.push_back(integer(key + ".burst", burst, "Requests allowed at once."));
    s.push_back(integer(key + ".per_minute", per_minute, "Requests allowed per minute after that."));
  }

  s.push_back(section("http.files", "A static file server, for the console."));
  s.push_back(boolean("http.files.enable", "true", "Serve files when the section is present."));
  s.push_back(text("http.files.path", "", "The directory served."));
  s.push_back(boolean("http.files.spa", "true",
                      "Answer a path that matches no file with index.html, so a single-page app's routes survive a reload. Never for "
                      "/api/ or a path with a file extension."));

  return s;
}

std::string parent_of(const std::string& key) {
  const auto dot = key.rfind('.');
  return dot == std::string::npos ? std::string() : key.substr(0, dot);
}

std::string last_of(const std::string& key) {
  const auto dot = key.rfind('.');
  return dot == std::string::npos ? key : key.substr(dot + 1);
}

std::size_t depth_of(const std::string& key) { return static_cast<std::size_t>(std::count(key.begin(), key.end(), '.')); }

std::size_t edit_distance(const std::string& a, const std::string& b) {
  std::vector<std::size_t> row(b.size() + 1);
  for (std::size_t j = 0; j <= b.size(); ++j) row[j] = j;

  for (std::size_t i = 1; i <= a.size(); ++i) {
    std::size_t diagonal = row[0];
    row[0] = i;
    for (std::size_t j = 1; j <= b.size(); ++j) {
      const auto above = row[j];
      row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
      diagonal = above;
    }
  }

  return row[b.size()];
}

boost::json::value default_of(const Setting& setting) {
  switch (setting.type) {
    case Setting::Type::Boolean:
      return setting.fallback == "true";
    case Setting::Type::Integer:
      return std::stoll(setting.fallback);
    case Setting::Type::List: {
      boost::json::array values;
      for (const auto& value : YAML::Load(setting.fallback)) values.emplace_back(value.as<std::string>());
      return values;
    }
    default:
      return boost::json::string(setting.fallback);
  }
}

boost::json::object schema_of(const Setting& setting) {
  boost::json::object schema;
  schema["description"] = setting.description;

  switch (setting.type) {
    case Setting::Type::Section:
      schema["type"] = "object";
      schema["additionalProperties"] = setting.open ? boost::json::value(boost::json::object{{"type", "object"}}) : boost::json::value(false);
      return schema;
    case Setting::Type::Boolean:
      schema["type"] = "boolean";
      break;
    case Setting::Type::Integer: {
      boost::json::object range{{"type", "integer"}};
      if (setting.minimum) range["minimum"] = *setting.minimum;
      if (setting.maximum) range["maximum"] = *setting.maximum;

      if (setting.or_zero) {
        schema["anyOf"] = boost::json::array{boost::json::object{{"const", 0}}, range};
      } else {
        for (const auto& [name, value] : range) schema[name] = value;
      }
      break;
    }
    case Setting::Type::String:
      schema["type"] = "string";
      break;
    case Setting::Type::Choice: {
      boost::json::array values;
      for (const auto& value : setting.choices) values.emplace_back(value);
      schema["enum"] = values;
      break;
    }
    case Setting::Type::List: {
      boost::json::value entry = boost::json::object{{"type", "string"}};
      if (!setting.entry_key.empty()) {
        entry = boost::json::object{
            {"anyOf", boost::json::array{boost::json::object{{"type", "string"}},
                                         boost::json::object{{"type", "object"},
                                                             {"properties", boost::json::object{{setting.entry_key, boost::json::object{{"type", "string"}}}}},
                                                             {"required", boost::json::array{boost::json::string(setting.entry_key)}}}}}};
      }

      if (setting.or_single) {
        schema["anyOf"] = boost::json::array{boost::json::object{{"type", "string"}}, boost::json::object{{"type", "array"}, {"items", entry}}};
      } else {
        schema["type"] = "array";
        schema["items"] = entry;
      }
      break;
    }
  }

  if (!setting.fallback.empty()) schema["default"] = default_of(setting);
  return schema;
}

// boost::json serializes on one line; a file a person reads is indented.
void pretty(std::ostringstream& out, const boost::json::value& value, int indent) {
  const std::string pad(static_cast<std::size_t>(indent + 2), ' ');
  const std::string close(static_cast<std::size_t>(indent), ' ');

  if (value.is_object()) {
    const auto& object = value.get_object();
    if (object.empty()) {
      out << "{}";
      return;
    }

    out << "{\n";
    bool first = true;
    for (const auto& [name, member] : object) {
      out << (first ? "" : ",\n") << pad << boost::json::serialize(boost::json::string(name)) << ": ";
      pretty(out, member, indent + 2);
      first = false;
    }
    out << "\n" << close << "}";
  } else if (value.is_array()) {
    const auto& array = value.get_array();
    if (array.empty()) {
      out << "[]";
      return;
    }

    out << "[\n";
    bool first = true;
    for (const auto& member : array) {
      out << (first ? "" : ",\n") << pad;
      pretty(out, member, indent + 2);
      first = false;
    }
    out << "\n" << close << "]";
  } else {
    out << boost::json::serialize(value);
  }
}

std::string type_text(const Setting& setting) {
  switch (setting.type) {
    case Setting::Type::Section:
      return "section";
    case Setting::Type::Boolean:
      return "true or false";
    case Setting::Type::Integer: {
      std::string range;
      if (setting.minimum && setting.maximum) {
        range = std::to_string(*setting.minimum) + " to " + std::to_string(*setting.maximum);
      } else if (setting.minimum) {
        range = std::to_string(*setting.minimum) + " or more";
      } else if (setting.maximum) {
        range = "up to " + std::to_string(*setting.maximum);
      }

      if (setting.or_zero) return "0, or " + range;
      return range.empty() ? "integer" : "integer, " + range;
    }
    case Setting::Type::String:
      return "text";
    case Setting::Type::List:
      return setting.or_single ? "list, or one value" : "list";
    case Setting::Type::Choice: {
      std::string values;
      for (const auto& value : setting.choices) values += (values.empty() ? "" : ", ") + ("`" + value + "`");
      return "one of " + values;
    }
  }
  return {};
}

// Puts each setting held directly in `prefix` into the object's properties, recursing into sections.
void fill(boost::json::object& object, const std::string& prefix, const Settings& settings) {
  boost::json::object properties;
  boost::json::array required;

  for (const auto& setting : settings) {
    if (parent_of(setting.key) != prefix) continue;

    auto schema = schema_of(setting);
    if (setting.type == Setting::Type::Section) fill(schema, setting.key, settings);
    properties[last_of(setting.key)] = std::move(schema);

    if (setting.required) required.emplace_back(last_of(setting.key));
  }

  // A section that can be turned off needs what it requires only when it is on.
  const bool switchable = properties.contains("enable");
  object["properties"] = std::move(properties);
  if (required.empty()) return;

  if (switchable) {
    object["if"] =
        boost::json::object{{"properties", boost::json::object{{"enable", boost::json::object{{"const", false}}}}}, {"required", boost::json::array{"enable"}}};
    object["else"] = boost::json::object{{"required", std::move(required)}};
  } else {
    object["required"] = std::move(required);
  }
}

void unknown_in(const YAML::Node& node, const std::string& prefix, const std::map<std::string, const Setting*>& by_key, const Settings& settings,
                std::vector<std::string>& found) {
  const auto here = by_key.find(prefix);
  const bool open = here != by_key.end() && here->second->open;

  for (const auto& entry : node) {
    const auto name = entry.first.as<std::string>();
    const auto key = prefix.empty() ? name : prefix + "." + name;

    if (const auto known = by_key.find(key); known != by_key.end()) {
      if (known->second->type == Setting::Type::Section && entry.second.IsMap()) unknown_in(entry.second, key, by_key, settings, found);
      continue;
    }

    // A driver's own section, whose keys this list does not hold.
    if (open && entry.second.IsMap()) continue;

    // The nearest key in the same section, or else the same name in another.
    std::string meant;
    std::size_t nearest = 3;
    for (const auto& setting : settings) {
      if (parent_of(setting.key) != prefix) continue;
      const auto distance = edit_distance(name, last_of(setting.key));
      if (distance < nearest) {
        nearest = distance;
        meant = setting.key;
      }
    }

    if (meant.empty()) {
      for (const auto& setting : settings) {
        if (last_of(setting.key) == name) {
          meant = setting.key;
          break;
        }
      }
    }

    found.push_back("'" + key + "' is not a setting, and is ignored" + (meant.empty() ? std::string() : " - did you mean '" + meant + "'?"));
  }
}

}  // namespace

const Settings& config_settings() {
  static const Settings settings = build();
  return settings;
}

std::string config_schema_json(const Settings& settings) {
  boost::json::object root{
      {"$schema", "https://json-schema.org/draft/2020-12/schema"},
      {"title", "AthenaSIP configuration"},
      {"description", "Generated by athenasip --print-schema. docs/configuration.md explains each section."},
      {"type", "object"},
      {"additionalProperties", false},
  };

  fill(root, "", settings);

  std::ostringstream out;
  pretty(out, root, 0);
  out << "\n";
  return out.str();
}

std::string config_reference_markdown(const Settings& settings) {
  std::ostringstream out;
  out << "# AthenaSIP - Configuration reference\n\n"
      << "Every setting, its default and its limits. [configuration.md](configuration.md) explains\n"
      << "them; this page is generated from the settings the server reads by\n"
      << "`athenasip --print-schema=markdown`, so edit `src/config_schema.cpp` rather than this.\n"
      << "`athenasip --print-schema` prints the same as a JSON Schema\n"
      << "([configuration.schema.json](configuration.schema.json)), which an editor can check a\n"
      << "file against as it is written.\n";

  for (const auto& owner : settings) {
    if (owner.type != Setting::Type::Section) continue;

    std::vector<const Setting*> held;
    for (const auto& setting : settings) {
      if (setting.type != Setting::Type::Section && parent_of(setting.key) == owner.key) held.push_back(&setting);
    }

    out << "\n" << std::string(std::min<std::size_t>(depth_of(owner.key) + 2, 4), '#') << " `" << owner.key << "`\n\n" << owner.description << "\n";
    if (held.empty()) continue;

    out << "\n| Setting | Takes | Default | |\n|---|---|---|---|\n";
    for (const auto* setting : held) {
      const auto fallback = setting->required ? std::string("required") : setting->fallback.empty() ? std::string("none") : "`" + setting->fallback + "`";
      out << "| `" << setting->key << "` | " << type_text(*setting) << " | " << fallback << " | " << setting->description << " |\n";
    }
  }

  return out.str();
}

std::vector<std::string> unknown_config_keys(const YAML::Node& root, const Settings& settings) {
  std::vector<std::string> found;
  if (!root || !root.IsMap()) return found;

  std::map<std::string, const Setting*> by_key;
  for (const auto& setting : settings) by_key[setting.key] = &setting;

  unknown_in(root, "", by_key, settings, found);
  return found;
}

}  // namespace athenasip
