//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <future>
#include <memory>
#include <string>
#include <vector>

#include "core.h"
#include "policy/policy.h"

namespace {

using namespace athenasip;

// The node's store and addresses, through Core as the real host reaches them, or a store that has failed.
class TestHost final : public policy::Host {
 public:
  TestHost(std::shared_ptr<Core> core) : _core(std::move(core)) {}

  bool down = false;

  void realm(std::string name, plugins::Handler<std::shared_ptr<types::Realm>> handler) override {
    if (down) return fail(handler);
    _core->realm_get_by_name(std::move(name), std::move(handler));
  }

  void subscriber(std::shared_ptr<types::SIPIdentity> identity, plugins::Handler<std::shared_ptr<types::Subscriber>> handler) override {
    if (down) return fail(handler);
    _core->subscriber_get(std::move(identity), std::move(handler));
  }

  void locations(std::uint64_t subscriber_id, plugins::Handler<std::vector<types::Location>> handler) override {
    if (down) return fail(handler);
    _core->location_list(subscriber_id, std::move(handler));
  }

  void trunk(std::string name, plugins::Handler<std::shared_ptr<types::Trunk>> handler) override {
    if (down) return fail(handler);
    _core->datastore->trunk_get(_core->strand(), std::move(name), std::move(handler));
  }

  void trunks(plugins::Handler<std::vector<std::shared_ptr<types::Trunk>>> handler) override {
    if (down) return fail(handler);
    _core->datastore->trunk_list(_core->strand(), std::move(handler));
  }

  bool names_this_node(const std::string& host, std::uint16_t port) const override { return _core->is_local_address(host, port); }

  const Config& config() const override { return *_core->config; }

 private:
  template <typename T>
  void fail(plugins::Handler<T> handler) {
    _core->post([handler]() { handler(plugins::Result<T>::failure("the store is down")); });
  }

  std::shared_ptr<Core> _core;
};

// Asks a policy one question from the strand, as the node does, and waits for the answer.
template <typename T, typename Ask>
plugins::Result<T> ask_policy(const std::shared_ptr<Core>& core, Ask ask) {
  std::promise<plugins::Result<T>> promise;
  auto future = promise.get_future();
  core->post([&]() { ask(core->strand(), [&promise](plugins::Result<T> result) { promise.set_value(std::move(result)); }); });
  return future.get();
}

}  // namespace
