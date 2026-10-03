//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio/ip/udp.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dns/message.h"
#include "loggers/logger.h"
#include "plugins/plugin.h"

namespace athenasip::dns {

// Asks a DNS server a question. The answer is the records of the type asked for: empty is
// a real answer - the name does not exist, or has nothing of that type - and a failure is
// only for not getting an answer at all. RFC 3263 treats the two differently: an empty
// answer moves it to the next step, and no answer is a hop it cannot reach.
//
// An interface so that what is built on it, the SIP locator, can be tested against canned
// answers rather than the network.
class Resolver {
 public:
  virtual ~Resolver() = default;

  virtual void query(plugins::Executor on, std::string name, Type type, plugins::Handler<std::vector<Record>> handler) = 0;
};

// The one that talks to real servers: a datagram to each configured server in turn, twice
// round as resolv.conf(5) does by default, and TCP when the answer did not fit in one
// (RFC 1035 4.2.2, RFC 7766). Every query has a socket of its own on a fresh port, so a
// reply is matched by where it came from and by its id, and one that matches neither is not
// an answer to anything this node asked.
class UdpResolver : public Resolver, public std::enable_shared_from_this<UdpResolver> {
 public:
  // Five seconds a try is resolv.conf(5)'s default. Shorter gives up on answers that are
  // merely slow: a public resolver fetching an SRV from a cold authoritative server takes
  // two and a half, measured, and a timeout under that turns every such lookup into a
  // retry.
  UdpResolver(std::shared_ptr<loggers::Logger> logger, std::vector<boost::asio::ip::udp::endpoint> servers,
              std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

  void query(plugins::Executor on, std::string name, Type type, plugins::Handler<std::vector<Record>> handler) override;

  // The nameservers in a resolv.conf (resolv.conf(5)): one per "nameserver" line, port 53,
  // comments and everything else ignored. Empty when there are none, or no file.
  static std::vector<boost::asio::ip::udp::endpoint> servers_from(const std::string& resolv_conf);

 private:
  std::shared_ptr<loggers::Logger> _logger;
  std::vector<boost::asio::ip::udp::endpoint> _servers;
  std::chrono::milliseconds _timeout;

  // RFC 1035 7.4: answers kept for their TTL, and an empty one - the name has nothing of
  // that type - for a short while (RFC 2308, without the SOA it would take the time from).
  // Queries are asked from the Core strand and answered on each query's own, so it is
  // locked.
  struct Cached {
    std::vector<Record> records;
    std::chrono::steady_clock::time_point expires;
  };

  std::mutex _cache_mutex;
  std::unordered_map<std::string, Cached> _cache;

  static std::string _cache_key(const std::string& name, Type type);
  bool _cached(const std::string& key, std::vector<Record>& out);
  void _keep(const std::string& key, const std::vector<Record>& records);

  struct Attempt;
  void _ask(std::shared_ptr<Attempt> attempt);
  void _ask_tcp(std::shared_ptr<Attempt> attempt);
  void _finish(std::shared_ptr<Attempt> attempt, plugins::Result<std::vector<Record>> result);
  void _next(std::shared_ptr<Attempt> attempt, const std::string& why);
};

}  // namespace athenasip::dns
