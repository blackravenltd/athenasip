//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace athenasip::dns {

// The DNS wire format (RFC 1035 section 4), as far as finding a SIP server needs it: a
// query for one name and type, and the answer section of the response, with the three
// record types RFC 3263 walks through. Hand-written per the dependency rule; the format is
// a header, length-prefixed labels with one compression rule, and fixed-width fields.

enum class Type : std::uint16_t {
  A = 1,       // RFC 1035 3.4.1
  AAAA = 28,   // RFC 3596
  SRV = 33,    // RFC 2782
  NAPTR = 35,  // RFC 3403
};

struct Srv {
  std::uint16_t priority = 0;
  std::uint16_t weight = 0;
  std::uint16_t port = 0;
  std::string target;
};

struct Naptr {
  std::uint16_t order = 0;
  std::uint16_t preference = 0;
  std::string flags;
  std::string services;
  std::string regexp;
  std::string replacement;
};

struct Record {
  std::string name;
  Type type = Type::A;
  std::uint32_t ttl = 0;

  // Which of these is set follows the type. An address is in its text form, which is what
  // a socket is asked to connect to.
  std::string address;
  Srv srv;
  Naptr naptr;
};

struct Response {
  std::uint16_t id = 0;
  bool truncated = false;

  // RFC 1035 4.1.1: 0 is no error and 3 is NXDOMAIN, the name does not exist.
  std::uint8_t rcode = 0;

  // The answer section, keeping only records of the types above. Anything else - a CNAME
  // the server followed for us, an OPT - is skipped rather than refused.
  std::vector<Record> answers;
};

// A standard query (opcode 0) for one name, recursion desired.
std::vector<std::uint8_t> encode_query(std::uint16_t id, const std::string& name, Type type);

// Nothing when the message is not a well-formed response: too short, a label or a
// compression pointer running off the end, or a pointer loop. A response off the network
// is anybody's to write.
std::optional<Response> decode_response(const std::vector<std::uint8_t>& message);

}  // namespace athenasip::dns
