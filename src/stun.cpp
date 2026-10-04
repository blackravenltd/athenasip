//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "stun.h"

namespace athenasip::stun {

namespace {

constexpr std::size_t kHeader = 20;
constexpr std::uint16_t kBindingRequest = 0x0001;
constexpr std::uint16_t kBindingSuccess = 0x0101;
constexpr std::uint16_t kXorMappedAddress = 0x0020;
const std::string kCookie("\x21\x12\xA4\x42", 4);

std::uint16_t u16(const std::string& s, std::size_t at) {
  return static_cast<std::uint16_t>((static_cast<unsigned char>(s[at]) << 8) | static_cast<unsigned char>(s[at + 1]));
}

void put16(std::string& out, std::uint16_t value) {
  out += static_cast<char>(value >> 8);
  out += static_cast<char>(value & 0xFF);
}

}  // namespace

bool is_stun(const std::string& datagram) {
  if (datagram.size() < kHeader) return false;
  if ((static_cast<unsigned char>(datagram[0]) & 0xC0) != 0) return false;
  if (datagram.compare(4, 4, kCookie) != 0) return false;

  const auto length = u16(datagram, 2);
  return length % 4 == 0 && length == datagram.size() - kHeader;
}

std::optional<std::string> binding_response(const std::string& request, const boost::asio::ip::address& address, std::uint16_t port) {
  if (!is_stun(request) || u16(request, 0) != kBindingRequest) return std::nullopt;

  const auto transaction = request.substr(8, 12);

  // RFC 5389 section 15.2 XOR-MAPPED-ADDRESS: the port XORed with the top of the cookie, an
  // IPv4 address with the cookie, an IPv6 address with the cookie and transaction id.
  std::string value;
  value += '\x00';
  value += static_cast<char>(address.is_v4() ? 0x01 : 0x02);
  put16(value, static_cast<std::uint16_t>(port ^ 0x2112));

  const auto mask = kCookie + transaction;
  if (address.is_v4()) {
    const auto bytes = address.to_v4().to_bytes();
    for (std::size_t i = 0; i < bytes.size(); ++i) value += static_cast<char>(bytes[i] ^ static_cast<unsigned char>(mask[i]));
  } else {
    const auto bytes = address.to_v6().to_bytes();
    for (std::size_t i = 0; i < bytes.size(); ++i) value += static_cast<char>(bytes[i] ^ static_cast<unsigned char>(mask[i]));
  }

  std::string response;
  put16(response, kBindingSuccess);
  put16(response, static_cast<std::uint16_t>(4 + value.size()));
  response += kCookie;
  response += transaction;
  put16(response, kXorMappedAddress);
  put16(response, static_cast<std::uint16_t>(value.size()));
  response += value;
  return response;
}

}  // namespace athenasip::stun
