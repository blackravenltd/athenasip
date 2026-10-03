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

// The plugin contract version. Every plugin reports the version it was built against
// and the registry refuses one that does not match, because a plugin built against a
// different contract is a crash rather than a warning. Bump it whenever anything in
// this file, or in an interface derived from Plugin, changes shape.
//
// 2 (2026-09-20): Datastore::subscriber_register takes the binding as a types::Location,
// so that the flow it was learned over and the node holding that flow can be recorded;
// and EventSystem's operations take the contract's Executor and handlers rather than a
// completion callback of their own shape, so that the contract is one contract.
//
// 3 (2026-09-21): types::Subscriber is types::Subscriber and every Datastore operation
// named after it follows, because SUBSCRIBE is a SIP method (RFC 6665) and the two
// would have collided the moment presence arrived. The shape of the calls is unchanged;
// only the word is. Redis keys changed with them, so a store provisioned under 2 is
// read as empty under 3 rather than half-read.
//
// 4 (2026-09-21): types::Subscriber carries a second credential, ha1_sha256, so a subscriber
// can answer a Digest challenge with SHA-256 (RFC 8760) as well as MD5. A store that
// does not carry it leaves subscribers able to authenticate with MD5 alone.
// 5 (2026-09-21): MediaEngine gains start_recording and stop_recording, so that the
// record capability a driver already advertises is one a caller can act on. Both
// decline by default, as the conference operations do, so a driver that does not record
// is unchanged.
// 6 (2026-09-22): media::Flags carries a target Profile, which says what the
// description the engine is about to produce has to be rather than what the one it was
// handed is. Only the caller knows: the transport of the flow a message is going out
// on is what distinguishes a browser from a desk phone before the browser has
// described itself, and an engine sees neither. Mirror is the default and is what
// every engine did before.
// 7 (2026-09-22): media::Flags::Profile gains SrtpSdes, for an endpoint that wants its
// media encrypted and has never heard of DTLS (RFC 4568). A driver that switches on
// the profile has a case it was not built for, which is what the version check is for.
// 8 (2026-10-01): types::Realm carries a Behaviour - optional settings over the server's
// default - in place of a MediaPolicy with every field set, so a datastore stores only
// what a realm chose. MediaEngine gains packets_relayed(), optional with a default, and a
// query document's `legs` are named in the contract.
// 9 (2026-10-02): types::Subscriber carries an optional media_profile, the operator's word
// about what the subscriber's endpoint is. A store that does not carry it loses the setting,
// and the subscriber's calls fall back to the realm's behaviour.
// 10 (2026-10-02): types::Behaviour carries an optional qualify_interval. A store that does
// not carry it loses the realm's choice, and the realm takes the server's default.
// 11 (2026-10-02): types::Behaviour carries an optional rewrite_contact, with the same
// consequence for a store that does not carry it.
// 12 (2026-10-02): types::Location carries RFC 5626 outbound's instance and reg_id, which a
// store has to keep for the registrar to tell a re-registered flow from a new one.
// 13 (2026-10-02): media::Flags carries an address for the leg a description is produced
// for, set for a leg inside sip.localnet. Empty is what every engine was given before.
// 14 (2026-10-03): types::Account is types::Subscriber and the Datastore operations named
// account_* are subscriber_*, with Location::account_id following. A rename and nothing
// else, but a driver built against 13 does not override what the server now calls.
inline constexpr std::uint32_t API_VERSION = 14;

// The kinds AthenaSIP registers today. A kind is a plain string rather than an enum so
// that a plugin can introduce one the core was not built knowing about.
namespace kinds {

inline constexpr char datastore[] = "datastore";
inline constexpr char events[] = "events";
inline constexpr char media[] = "media";

}  // namespace kinds

// The executor a caller hands a plugin so that its completion handler runs back where
// the caller lives. Core hands in its strand; a plugin never decides for itself which
// thread application code runs on.
using Executor = boost::asio::any_io_executor;

// What an operation that returns nothing but can fail reports back.
struct Status {
  bool ok = false;
  std::string error;

  static Status success() { return Status{true, {}}; }
  static Status failure(std::string reason) { return Status{false, std::move(reason)}; }

  explicit operator bool() const { return ok; }
};

// What an operation that returns something reports back. A read that finds nothing is
// a success carrying an empty value, not a failure: "no such realm" and "Redis is
// down" are different answers and the caller has to tell them apart.
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

// What every plugin is, whatever it plugs into. Datastore, EventSystem and MediaEngine
// derive from this, and so does anything added later; the registry knows nothing else
// about them.
//
// Identity and configuration live here. The work a kind does lives on the interface for
// that kind.
class Plugin {
 public:
  virtual ~Plugin() = default;

  // What this plugs into: one of the kinds:: constants, or a kind of the plugin's own.
  virtual std::string kind() const = 0;

  // The driver's own name, short and stable: "memory", "redis", "rtpengine". It names
  // the implementation rather than the scheme it was reached by, so all three Redis
  // schemes answer "redis".
  virtual std::string name() const = 0;

  // The driver's version, which has nothing to do with the server's.
  virtual std::string version() const = 0;

  // The contract this driver was built against. A driver compiled into the server
  // answers the version it was compiled with, which is what makes the check trivial
  // now and meaningful once plugins are loaded from shared libraries.
  virtual std::uint32_t api_version() const { return API_VERSION; }

  // Configuration, called once before connect(). own_root is the config section named
  // after the driver and is empty when there is none; system is the whole server
  // configuration, for what a driver cannot be told twice (node id, timers). The URL
  // stays the selector, so a driver needing nothing else can ignore both and a
  // one-line datastore: { url: memory:// } keeps working.
  virtual bool configure(const YAML::Node& own_root, const Config& system) {
    (void)own_root;
    (void)system;
    return true;
  }

  // Whether the driver is serving right now. Cheap and non-blocking: it reports what
  // the driver already knows rather than going and asking the far end.
  virtual bool health() const { return true; }

  // For logs and the admin API: "redis 0.0.1".
  std::string describe() const { return name() + " " + version(); }

 protected:
  // How a driver answers. The handler is posted to the caller's executor and never
  // called inline, so a caller is never re-entered from inside its own call. The
  // in-process drivers could answer immediately and the networked ones cannot, and
  // code written against the first has to work against the second.
  template <typename H, typename V>
  static void _complete(Executor on, H handler, V value) {
    if (!handler) return;

    boost::asio::post(std::move(on), [handler = std::move(handler), value = std::move(value)]() mutable { handler(std::move(value)); });
  }

  // For an operation whose only failure is "it did not happen": the name is what goes
  // in the error, so a caller logging it says which call failed.
  static Status _status(bool ok, std::string operation) { return ok ? Status::success() : Status::failure(std::move(operation) + " failed"); }
};

}  // namespace athenasip::plugins
