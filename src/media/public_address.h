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

// The address a media engine advertises, which may be a name: a node behind NAT on a
// dynamic address advertises the name a dynamic DNS updater keeps pointing at the site.
// What goes into a description is an address, because phones do not resolve a name in a
// c= line, so a name is looked up when the engine starts and every minute after, off the
// call path, and media follows the name when the address changes.
//
// An address is used as it is. A name that resolves to nothing gives nothing, and the
// engine declines rather than write the name.
class PublicAddress {
 public:
  explicit PublicAddress(std::shared_ptr<loggers::Logger> logger);
  ~PublicAddress();

  // The configured value, an address or a name. Empty is none.
  void set(const std::string& configured);
  const std::string& configured() const { return _configured; }

  // A name is resolved now, which blocks: this is the engine connecting, before the node
  // serves anything. Then again every minute in the background.
  void start();
  void stop();

  // What to write: the address, or what the name last resolved to. Empty when a name has
  // not resolved, and when nothing was configured.
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
