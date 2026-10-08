//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "trunk.h"

#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/network_v4.hpp>
#include <boost/asio/ip/network_v6.hpp>
#include <stdexcept>

#include "../util.h"

namespace athenasip::types {

namespace {

std::string text(const boost::json::object& stored, const char* key) {
  const auto* value = stored.if_contains(key);
  return value != nullptr && value->is_string() ? std::string(value->as_string()) : std::string();
}

// A range as an address and a prefix length; a bare address is the whole length.
bool parse_range(const std::string& range, boost::asio::ip::address& address, unsigned& prefix) {
  const auto slash = range.find('/');
  boost::system::error_code error;
  address = boost::asio::ip::make_address(range.substr(0, slash), error);
  if (error) return false;

  const unsigned whole = address.is_v4() ? 32 : 128;
  if (slash == std::string::npos) {
    prefix = whole;
    return true;
  }

  const auto length = range.substr(slash + 1);
  if (length.empty() || length.size() > 3 || length.find_first_not_of("0123456789") != std::string::npos) return false;
  prefix = static_cast<unsigned>(std::stoul(length));
  return prefix <= whole;
}

}  // namespace

std::string Trunk::normalise(const std::string& name) { return Util::to_lower(name); }

bool Trunk::valid_name(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  for (const char c : name) {
    const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    if (!word) return false;
  }
  return true;
}

bool Trunk::valid_range(const std::string& range) {
  boost::asio::ip::address address;
  unsigned prefix = 0;
  return parse_range(range, address, prefix);
}

bool in_range(const std::string& range, const std::string& address) {
  boost::asio::ip::address network;
  unsigned prefix = 0;
  if (!parse_range(range, network, prefix)) return false;

  boost::system::error_code error;
  auto candidate = boost::asio::ip::make_address(address, error);
  if (error) return false;

  // An IPv4 peer seen on a dual-stack socket arrives as ::ffff:a.b.c.d.
  if (candidate.is_v6() && candidate.to_v6().is_v4_mapped()) candidate = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, candidate.to_v6());

  if (network.is_v4() != candidate.is_v4()) return false;
  if (network.is_v4()) {
    return boost::asio::ip::make_network_v4(candidate.to_v4(), prefix).canonical() == boost::asio::ip::make_network_v4(network.to_v4(), prefix).canonical();
  }
  return boost::asio::ip::make_network_v6(candidate.to_v6(), prefix).canonical() == boost::asio::ip::make_network_v6(network.to_v6(), prefix).canonical();
}

bool Trunk::admits(const std::string& address) const {
  for (const auto& range : inbound_addresses) {
    if (in_range(range, address)) return true;
  }
  return false;
}

boost::json::object Trunk::to_json() const {
  boost::json::object out;
  out["name"] = name;
  out["uri"] = uri;
  out["proxy"] = proxy;
  out["username"] = username;
  out["password"] = password;
  out["register"] = {{"enabled", register_enabled}, {"expires", register_expires}, {"contact_user", contact_user}};

  boost::json::array inbound;
  for (const auto& range : inbound_addresses) inbound.push_back(boost::json::string(range));
  out["inbound_addresses"] = std::move(inbound);

  out["tls_ca"] = tls_ca;
  out["attributes"] = attributes;
  out["created_at"] = static_cast<std::int64_t>(created_at);
  return out;
}

Trunk Trunk::from_json(const boost::json::object& stored) {
  Trunk trunk;
  trunk.name = text(stored, "name");
  if (trunk.name.empty()) throw std::runtime_error("a trunk with no name");

  trunk.uri = text(stored, "uri");
  trunk.proxy = text(stored, "proxy");
  trunk.username = text(stored, "username");
  trunk.password = text(stored, "password");
  trunk.tls_ca = text(stored, "tls_ca");

  if (const auto* registration = stored.if_contains("register"); registration != nullptr && registration->is_object()) {
    const auto& r = registration->as_object();
    if (const auto* enabled = r.if_contains("enabled"); enabled != nullptr && enabled->is_bool()) trunk.register_enabled = enabled->as_bool();
    if (const auto* expires = r.if_contains("expires"); expires != nullptr && expires->is_number()) {
      trunk.register_expires = expires->to_number<std::uint32_t>();
    }
    trunk.contact_user = text(r, "contact_user");
  }

  if (const auto* inbound = stored.if_contains("inbound_addresses"); inbound != nullptr && inbound->is_array()) {
    for (const auto& range : inbound->as_array()) {
      if (range.is_string()) trunk.inbound_addresses.emplace_back(range.as_string());
    }
  }

  if (const auto* attributes = stored.if_contains("attributes"); attributes != nullptr && attributes->is_object()) trunk.attributes = attributes->as_object();
  if (const auto* created = stored.if_contains("created_at"); created != nullptr && created->is_number()) {
    trunk.created_at = static_cast<std::time_t>(created->to_number<std::int64_t>());
  }
  return trunk;
}

}  // namespace athenasip::types
