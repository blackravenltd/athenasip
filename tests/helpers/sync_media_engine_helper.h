//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstddef>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "global_io_context.h"
#include "media/media_engine.h"
#include "plugins/plugin.h"

// A blocking view of a media engine, for tests only (see SyncDatastore).
class SyncMediaEngine {
 public:
  explicit SyncMediaEngine(std::shared_ptr<athenasip::media::MediaEngine> engine) : _engine(std::move(engine)) {}

  // Synchronous in the contract.
  std::string name() const { return _engine->name(); }
  std::string version() const { return _engine->version(); }
  bool is_connected() const { return _engine->is_connected(); }
  athenasip::media::Capabilities capabilities() const { return _engine->capabilities(); }
  void close() { _engine->close(); }

  bool configure(const YAML::Node& own_root, const athenasip::Config& system) { return _engine->configure(own_root, system); }

  std::shared_ptr<athenasip::media::MediaEngine> driver() const { return _engine; }

  bool connect() {
    std::promise<athenasip::plugins::Status> promise;
    auto future = promise.get_future();

    _engine->connect(_executor(), [&promise](athenasip::plugins::Status status) { promise.set_value(std::move(status)); });

    return future.get().ok;
  }

  athenasip::media::Result offer(std::shared_ptr<athenasip::Call> call, const std::string& sdp, const athenasip::media::Flags& flags) {
    return _media([&](athenasip::media::MediaEngine::MediaHandler handler) { _engine->offer(_executor(), call, sdp, flags, std::move(handler)); });
  }

  athenasip::media::Result answer(std::shared_ptr<athenasip::Call> call, const std::string& sdp, const athenasip::media::Flags& flags) {
    return _media([&](athenasip::media::MediaEngine::MediaHandler handler) { _engine->answer(_executor(), call, sdp, flags, std::move(handler)); });
  }

  athenasip::plugins::Status release_status(std::shared_ptr<athenasip::Call> call) {
    std::promise<athenasip::plugins::Status> promise;
    auto future = promise.get_future();

    _engine->release(_executor(), std::move(call), [&promise](athenasip::plugins::Status status) { promise.set_value(std::move(status)); });

    return future.get();
  }

  athenasip::plugins::Result<std::string> query_result(std::shared_ptr<athenasip::Call> call) {
    std::promise<athenasip::plugins::Result<std::string>> promise;
    auto future = promise.get_future();

    _engine->query(_executor(), std::move(call), [&promise](athenasip::plugins::Result<std::string> result) { promise.set_value(std::move(result)); });

    return future.get();
  }

  athenasip::plugins::Status start_recording(std::shared_ptr<athenasip::Call> call) {
    std::promise<athenasip::plugins::Status> promise;
    auto future = promise.get_future();

    _engine->start_recording(_executor(), std::move(call), [&promise](athenasip::plugins::Status status) { promise.set_value(std::move(status)); });

    return future.get();
  }

  athenasip::plugins::Status stop_recording(std::shared_ptr<athenasip::Call> call) {
    std::promise<athenasip::plugins::Status> promise;
    auto future = promise.get_future();

    _engine->stop_recording(_executor(), std::move(call), [&promise](athenasip::plugins::Status status) { promise.set_value(std::move(status)); });

    return future.get();
  }

  bool release(std::shared_ptr<athenasip::Call> call) {
    std::promise<athenasip::plugins::Status> promise;
    auto future = promise.get_future();

    _engine->release(_executor(), std::move(call), [&promise](athenasip::plugins::Status status) { promise.set_value(std::move(status)); });

    return future.get().ok;
  }

  std::string query(std::shared_ptr<athenasip::Call> call) {
    std::promise<athenasip::plugins::Result<std::string>> promise;
    auto future = promise.get_future();

    _engine->query(_executor(), std::move(call), [&promise](athenasip::plugins::Result<std::string> result) { promise.set_value(std::move(result)); });

    return future.get().value;
  }

  bool join(std::shared_ptr<athenasip::Call> call, std::size_t participant) {
    std::promise<athenasip::plugins::Status> promise;
    auto future = promise.get_future();

    _engine->join(_executor(), std::move(call), participant, [&promise](athenasip::plugins::Status status) { promise.set_value(std::move(status)); });

    return future.get().ok;
  }

  bool leave(std::shared_ptr<athenasip::Call> call, std::size_t participant) {
    std::promise<athenasip::plugins::Status> promise;
    auto future = promise.get_future();

    _engine->leave(_executor(), std::move(call), participant, [&promise](athenasip::plugins::Status status) { promise.set_value(std::move(status)); });

    return future.get().ok;
  }

  std::vector<std::string> roster(std::shared_ptr<athenasip::Call> call) {
    std::promise<athenasip::plugins::Result<std::vector<std::string>>> promise;
    auto future = promise.get_future();

    _engine->roster(_executor(), std::move(call),
                    [&promise](athenasip::plugins::Result<std::vector<std::string>> result) { promise.set_value(std::move(result)); });

    return future.get().value;
  }

 private:
  template <typename Start>
  athenasip::media::Result _media(Start start) {
    std::promise<athenasip::media::Result> promise;
    auto future = promise.get_future();

    start([&promise](athenasip::media::Result result) { promise.set_value(std::move(result)); });

    return future.get();
  }

  static athenasip::plugins::Executor _executor() { return athenasip::detail::get_global_io_context().get_executor(); }

  std::shared_ptr<athenasip::media::MediaEngine> _engine;
};
