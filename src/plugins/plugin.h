//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <yaml-cpp/yaml.h>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace athenasip {
class Config;
}

namespace athenasip::plugins {

// The plugin contract version. A plugin reports the version it was built against and the
// registry refuses a mismatch. Bump it whenever anything in this file, or in an interface
// derived from Plugin, changes shape.
inline constexpr std::uint32_t API_VERSION = 15;

// The kinds AthenaSIP registers. A kind is a string, not an enum, so a plugin can
// introduce its own.
namespace kinds {

inline constexpr char datastore[] = "datastore";
inline constexpr char events[] = "events";
inline constexpr char media[] = "media";

}  // namespace kinds

// The executor a caller hands a plugin; completion handlers run on it. Core passes its
// strand.
using Executor = boost::asio::any_io_executor;

// The outcome of an operation that returns no value.
struct Status {
  bool ok = false;
  std::string error;

  static Status success() { return Status{true, {}}; }
  static Status failure(std::string reason) { return Status{false, std::move(reason)}; }

  explicit operator bool() const { return ok; }
};

// The outcome of an operation that returns a value. A read that finds nothing succeeds
// with an empty value; failure means the store could not answer.
template <typename T>
struct Result {
  bool ok = false;
  std::string error;
  T value{};

  static Result success(T value) { return Result{true, {}, std::move(value)}; }
  static Result failure(std::string reason) { return Result{false, std::move(reason), T{}}; }

  explicit operator bool() const { return ok; }
};

template <typename T>
using Handler = std::function<void(Result<T>)>;

using StatusHandler = std::function<void(Status)>;

// The base of every plugin: identity and configuration. Datastore, EventSystem and
// MediaEngine derive from it and add the operations of their kind.
class Plugin {
 public:
  virtual ~Plugin() = default;

  // One of the kinds:: constants, or a kind of the plugin's own.
  virtual std::string kind() const = 0;

  // The implementation's short, stable name ("memory", "redis", "rtpengine"), whatever
  // scheme it was reached by.
  virtual std::string name() const = 0;

  // The driver's own version, independent of the server's.
  virtual std::string version() const = 0;

  // The contract version this driver was built against.
  virtual std::uint32_t api_version() const { return API_VERSION; }

  // Called once before connect(). own_root is the config section named after the driver,
  // empty when there is none; system is the whole server configuration (node id, timers).
  // A driver that needs only its URL can ignore both.
  virtual bool configure(const YAML::Node& own_root, const Config& system) {
    (void)own_root;
    (void)system;
    return true;
  }

  // Whether the driver is serving. Cheap and non-blocking: report what is already known.
  virtual bool health() const { return true; }

  // For logs and the admin API: "redis 0.0.1".
  std::string describe() const { return name() + " " + version(); }

 protected:
  // How a driver answers: the handler is posted to the caller's executor, never called
  // inline, so a caller is never re-entered from inside its own call.
  template <typename H, typename V>
  static void _complete(Executor on, H handler, V value) {
    if (!handler) return;

    boost::asio::post(std::move(on), [handler = std::move(handler), value = std::move(value)]() mutable { handler(std::move(value)); });
  }

  // A Status for an operation whose only failure is "<operation> failed".
  static Status _status(bool ok, std::string operation) { return ok ? Status::success() : Status::failure(std::move(operation) + " failed"); }
};

}  // namespace athenasip::plugins
