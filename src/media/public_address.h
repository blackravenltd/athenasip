//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio/steady_timer.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "../loggers/logger.h"

namespace athenasip::media {

// The address a media engine advertises. It may be a name, for a node behind NAT on a dynamic address; SDP needs an
// address, so a name is resolved at start and every minute after, off the call path.
//
// An address is used as is. A name that does not resolve yields empty, and the engine declines.
class PublicAddress {
 public:
  explicit PublicAddress(std::shared_ptr<loggers::Logger> logger);
  ~PublicAddress();

  // The configured value, an address or a name. Empty is none.
  void set(const std::string& configured);
  const std::string& configured() const { return _configured; }

  // Resolves a name once, blocking (the engine is connecting; nothing is served yet), then every minute in the background.
  void start();
  void stop();

  // The address to write. Empty when nothing is configured or a name has not resolved.
  std::string current() const;

  bool is_name() const;

 private:
  struct State {
    std::mutex mutex;
    std::string name;
    std::string address;
  };

  void _schedule();

  std::shared_ptr<loggers::Logger> _logger;
  std::string _configured;
  std::shared_ptr<State> _state = std::make_shared<State>();
  std::shared_ptr<boost::asio::steady_timer> _timer;
  std::shared_ptr<std::function<void()>> _tick;
};

}  // namespace athenasip::media
