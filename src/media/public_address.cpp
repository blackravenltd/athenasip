//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "public_address.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>
#include <chrono>

#include "../global_io_context.h"

namespace athenasip::media {

namespace {

using udp = boost::asio::ip::udp;

}  // namespace

PublicAddress::PublicAddress(std::shared_ptr<loggers::Logger> logger) : _logger(std::move(logger)) {}

PublicAddress::~PublicAddress() { stop(); }

void PublicAddress::set(const std::string& configured) { _configured = configured; }

bool PublicAddress::is_name() const {
  if (_configured.empty()) return false;

  boost::system::error_code literal;
  boost::asio::ip::make_address(_configured, literal);
  return static_cast<bool>(literal);
}

std::string PublicAddress::current() const {
  if (!is_name()) return _configured;

  std::lock_guard<std::mutex> lock(_state->mutex);
  return _state->address;
}

void PublicAddress::start() {
  if (!is_name()) return;

  {
    std::lock_guard<std::mutex> lock(_state->mutex);
    _state->name = _configured;
  }

  boost::asio::io_context io;
  udp::resolver resolver(io);
  boost::system::error_code failed;
  const auto found = resolver.resolve(udp::v4(), _configured, "", failed);

  if (!failed && !found.empty()) {
    std::lock_guard<std::mutex> lock(_state->mutex);
    _state->address = found.begin()->endpoint().address().to_string();
    _logger->info("Public address " + _configured + " is " + _state->address);
  } else {
    _logger->error("Public address " + _configured + " does not resolve - media will not be anchored until it does");
  }

  _schedule();
}

void PublicAddress::stop() {
  if (_timer) _timer->cancel();
  _timer.reset();
  _tick.reset();
}

// Every minute, on the process's own io_context and never on the Core strand: a lookup is
// a network round trip. A change is logged, because it is the site's address moving. This
// object owns the loop and each step holds it weakly, so stopping ends it.
void PublicAddress::_schedule() {
  _timer = std::make_shared<boost::asio::steady_timer>(detail::get_global_io_context());
  _tick = std::make_shared<std::function<void()>>();

  std::weak_ptr<State> weak_state = _state;
  std::weak_ptr<boost::asio::steady_timer> weak_timer = _timer;
  std::weak_ptr<std::function<void()>> weak_tick = _tick;
  auto logger = _logger;

  *_tick = [weak_state, weak_timer, weak_tick, logger]() {
    auto timer = weak_timer.lock();
    if (!timer) return;

    timer->expires_after(std::chrono::seconds(60));
    timer->async_wait([weak_state, weak_tick, logger](boost::system::error_code ec) {
      if (ec) return;
      auto state = weak_state.lock();
      if (!state) return;

      std::string name;
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        name = state->name;
      }

      auto resolver = std::make_shared<udp::resolver>(detail::get_global_io_context());
      resolver->async_resolve(udp::v4(), name, "",
                              [resolver, weak_state, weak_tick, logger, name](boost::system::error_code failed, udp::resolver::results_type found) {
                                if (auto state = weak_state.lock(); state && !failed && !found.empty()) {
                                  const auto address = found.begin()->endpoint().address().to_string();
                                  std::lock_guard<std::mutex> lock(state->mutex);
                                  if (address != state->address) {
                                    logger->info("Public address " + name + " is now " + address + (state->address.empty() ? "" : ", was " + state->address));
                                    state->address = address;
                                  }
                                }

                                if (auto next = weak_tick.lock()) (*next)();
                              });
    });
  };

  (*_tick)();
}

}  // namespace athenasip::media
