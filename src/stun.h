//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio/ip/address.hpp>
#include <cstdint>
#include <optional>
#include <string>

// The STUN server RFC 5626 section 4.4.2 requires of a SIP node: clients keep a UDP flow
// alive with Binding requests to the SIP port, and each is answered with its source
// address. No authentication, other methods or attributes are handled.
namespace athenasip::stun {

// Whether a datagram is a STUN message (RFC 5389 section 6): top two bits clear, the magic
// cookie, and a length that matches the datagram and is a multiple of four.
bool is_stun(const std::string& datagram);

// The Binding success response to a Binding request from address:port, or nullopt for
// anything else.
std::optional<std::string> binding_response(const std::string& request, const boost::asio::ip::address& address, std::uint16_t port);

// RFC 5389 6: a Binding request with no attributes, carrying a 12-byte transaction id.
std::string binding_request(const std::string& transaction_id);

// What a Binding success response says the request came from: XOR-MAPPED-ADDRESS (15.2), else MAPPED-ADDRESS
// (15.1) from a pre-5389 server. Nothing for anything else.
struct Mapped {
  boost::asio::ip::address address;
  std::uint16_t port = 0;
  std::string transaction_id;
};
std::optional<Mapped> binding_success(const std::string& datagram);

}  // namespace athenasip::stun
