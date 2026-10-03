//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "dns/resolver.h"
#include "plugins/plugin.h"
#include "types/sip_uri.h"

namespace athenasip::dns {

// One place to try a request: a transport, an address and a port.
struct Hop {
  std::string transport;  // "udp", "tcp" or "tls"
  std::string address;
  std::uint16_t port = 0;

  friend bool operator==(const Hop&, const Hop&) = default;
};

// RFC 3263 section 4: where a request for a SIP URI goes, as an ordered list of hops to try
// in turn (4.3). NAPTR picks the transport, SRV the port and the hosts, A and AAAA the
// addresses, and each step is skipped when the URI already says what it would have found.
//
// Empty is an answer: the name exists and nothing serves SIP at it, or it does not exist.
// A failure is only for not getting answers at all.
class SipLocator : public std::enable_shared_from_this<SipLocator> {
 public:
  // The source of randomness for RFC 2782's weighted choice, in [0, total]. Injectable so a
  // test can say which way the dice fall.
  using Random = std::function<std::uint32_t(std::uint32_t total)>;

  explicit SipLocator(std::shared_ptr<Resolver> resolver, Random random = {});

  void locate(plugins::Executor on, const types::SIPUri& uri, plugins::Handler<std::vector<Hop>> handler);

 private:
  std::shared_ptr<Resolver> _resolver;
  Random _random;

  struct Search;
  void _naptr(std::shared_ptr<Search> search);
  void _srv_in_turn(std::shared_ptr<Search> search, std::vector<std::pair<std::string, std::string>> names, std::size_t next);
  void _srv_targets(std::shared_ptr<Search> search, std::vector<std::pair<std::string, Srv>> targets, std::size_t index,
                    std::vector<std::pair<std::string, std::string>> names, std::size_t next);
  void _addresses(std::shared_ptr<Search> search, std::vector<std::pair<std::string, Srv>> targets, std::size_t next);
  void _finish(std::shared_ptr<Search> search);

  std::vector<Srv> _order(std::vector<Srv> records) const;
};

}  // namespace athenasip::dns
