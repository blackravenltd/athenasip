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

// The STUN server RFC 5626 section 4.4.2 asks of a SIP node: a client keeps a UDP flow alive
// with STUN Binding requests sent to the SIP port, and the node answers each with where it
// came from. That and nothing else - no authentication, no other methods, no attributes
// read - which is the whole of what the keep-alive needs and why it is written here rather
// than taken from a library.
namespace athenasip::stun {

// Whether a datagram is a STUN message (RFC 5389 section 6): the top two bits clear, the
// magic cookie, and a length that is the rest of the datagram and a multiple of four. SIP
// starts with a letter or a CRLF, so the two are told apart by the first byte alone.
bool is_stun(const std::string& datagram);

// The Binding success response to a Binding request from address:port, or nothing for
// anything that is not a Binding request.
std::optional<std::string> binding_response(const std::string& request, const boost::asio::ip::address& address, std::uint16_t port);

}  // namespace athenasip::stun
