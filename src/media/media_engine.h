//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../call.h"
#include "../loggers/logger.h"
#include "../plugins/plugin.h"
#include "../plugins/plugin_registry.h"
#include "../types/url.h"

namespace athenasip::media {

// What an engine can do. A driver advertises these rather than the caller assuming:
// the builtin relay can only bridge, rtpengine can also record and transcode, and an
// SFU adds conference.
struct Capabilities {
  bool bridge = false;      // Relay media between participants
  bool conference = false;  // Mix or forward for three or more participants
  bool record = false;      // Record a call
  bool transcode = false;   // Convert between codecs

  bool supports_all_of(const Capabilities& required) const {
    return (!required.bridge || bridge) && (!required.conference || conference) && (!required.record || record) && (!required.transcode || transcode);
  }
};

// Per-operation options, derived from the SDP rather than from the transport. A WebRTC
// offer needs ICE and DTLS whatever it arrived over, and a plain RTP endpoint needs
// neither (see the M3 notes in TODO/ACTIVE.md).
struct Flags {
  bool ice = false;
  bool dtls = false;
  bool srtp = false;
  bool rtcp_mux = false;

  // The participant this offer or answer belongs to, as an index into
  // Call::participants. A multi-party call has more than two.
  std::size_t participant = 0;
};

// The result of an offer or answer: the SDP to pass on, and whether the engine took
// the media or declined it.
struct Result {
  bool ok = false;
  std::string sdp;
  std::string error;

  static Result failure(std::string reason) {
    Result result;
    result.error = std::move(reason);
    return result;
  }

  static Result success(std::string sdp) {
    Result result;
    result.ok = true;
    result.sdp = std::move(sdp);
    return result;
  }
};

// A media engine anchors, bridges or mixes the media for a call. It is not a two-party
// SDP rewriter: offer and answer name the participant they belong to, and a
// conference-capable driver takes join, leave and roster as well.
//
// Drivers register by URL scheme, the same way Datastore and EventSystem do, so the
// engine is chosen by configuration: builtin:// or rtpengine://host:port.
//
// Async for the same reason the datastore is: rtpengine is an ng-protocol round trip
// over UDP, and the builtin relay only looks instant because it happens to be in this
// process. The caller is on the Core strand and must not wait there, and the contract
// cannot be made async later without breaking every engine written against it.
class MediaEngine : public plugins::Plugin {
 public:
  ~MediaEngine() override = default;

  std::string kind() const final { return plugins::kinds::media; }

  // What an offer or answer reports back. Result already carries ok and error, so it
  // is the handler's argument as it stands.
  using MediaHandler = std::function<void(Result)>;

  // Lifecycle. connect() is a round trip for a networked engine and is async like
  // everything else; close() is teardown, synchronous and safe to call twice.
  virtual void connect(plugins::Executor on, plugins::StatusHandler handler) = 0;
  virtual void close() = 0;
  virtual bool is_connected() const = 0;

  // What this driver can do is local knowledge and needs no round trip.
  virtual Capabilities capabilities() const = 0;

  // Two-way media setup. offer() takes the offer from one participant and answers with
  // the SDP to send on; answer() takes the answer coming back.
  virtual void offer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) = 0;
  virtual void answer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) = 0;

  // Give up every resource held for this call. Safe to call for a call the engine
  // never saw.
  virtual void release(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) = 0;

  // What the engine currently holds for this call, for the admin API and diagnostics.
  virtual void query(plugins::Executor on, std::shared_ptr<Call> call, plugins::Handler<std::string> handler) = 0;

  // Conference operations. Only meaningful when capabilities().conference is set; the
  // defaults below decline, so a bridge-only driver does not have to implement them.
  virtual void join(plugins::Executor on, std::shared_ptr<Call> call, std::size_t participant, plugins::StatusHandler handler) {
    (void)call;
    (void)participant;
    _complete(std::move(on), std::move(handler), plugins::Status::failure("this engine does not do conferences"));
  }

  virtual void leave(plugins::Executor on, std::shared_ptr<Call> call, std::size_t participant, plugins::StatusHandler handler) {
    (void)call;
    (void)participant;
    _complete(std::move(on), std::move(handler), plugins::Status::failure("this engine does not do conferences"));
  }

  virtual void roster(plugins::Executor on, std::shared_ptr<Call> call, plugins::Handler<std::vector<std::string>> handler) {
    (void)call;
    _complete(std::move(on), std::move(handler), plugins::Result<std::vector<std::string>>::success({}));
  }

  template <typename T, typename = std::enable_if_t<std::is_base_of_v<MediaEngine, T>>>
  static void register_driver(std::shared_ptr<loggers::Logger> logger, std::string scheme) {
    plugins::PluginRegistry::instance().add<T>(std::move(logger), plugins::kinds::media, std::move(scheme));
  }

  static std::shared_ptr<MediaEngine> create_driver(std::shared_ptr<loggers::Logger> logger, const std::string& url_string) {
    return plugins::PluginRegistry::instance().create_as<MediaEngine>(std::move(logger), plugins::kinds::media, url_string);
  }
};

}  // namespace athenasip::media
