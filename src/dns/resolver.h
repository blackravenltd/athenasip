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

// Asks a DNS server a question. An empty answer is an answer: the name does not exist, or has nothing of that type.
// A failure means no answer at all. RFC 3263 moves on to its next step after the first and gives up the hop after the
// second.
//
// An interface so the SIP locator can be tested against canned answers.
class Resolver {
 public:
  virtual ~Resolver() = default;

  virtual void query(plugins::Executor on, std::string name, Type type, plugins::Handler<std::vector<Record>> handler) = 0;
};

// Talks to real servers: a datagram to each configured server in turn, twice round as resolv.conf(5) does, and TCP
// when the answer is truncated (RFC 1035 4.2.2, RFC 7766). Each query has its own socket on a fresh port; a reply
// is matched by its source and its id.
class UdpResolver : public Resolver, public std::enable_shared_from_this<UdpResolver> {
 public:
  // Five seconds per try is resolv.conf(5)'s default. A public resolver fetching an SRV from a cold authoritative
  // server can take over two seconds, so a much shorter timeout turns slow answers into retries.
  UdpResolver(std::shared_ptr<loggers::Logger> logger, std::vector<boost::asio::ip::udp::endpoint> servers,
              std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

  void query(plugins::Executor on, std::string name, Type type, plugins::Handler<std::vector<Record>> handler) override;

  // The "nameserver" lines of a resolv.conf (resolv.conf(5)), port 53. Empty when there are none, or no file.
  static std::vector<boost::asio::ip::udp::endpoint> servers_from(const std::string& resolv_conf);

 private:
  std::shared_ptr<loggers::Logger> _logger;
  std::vector<boost::asio::ip::udp::endpoint> _servers;
  std::chrono::milliseconds _timeout;

  // RFC 1035 7.4: answers are cached for their TTL, and an empty answer briefly (RFC 2308). Locked, because queries are
  // asked from the Core strand and answered on each query's own.
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
