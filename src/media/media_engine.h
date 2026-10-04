//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../call.h"
#include "../loggers/logger.h"
#include "../plugins/plugin.h"
#include "../plugins/plugin_registry.h"
#include "../types/url.h"
#include "media_profile.h"

namespace athenasip::media {

// What an engine can do. Drivers advertise it; callers must not assume.
struct Capabilities {
  bool bridge = false;      // Relay media between participants
  bool conference = false;  // Mix or forward for three or more participants
  bool record = false;      // Record a call
  bool transcode = false;   // Convert between codecs

  bool supports_all_of(const Capabilities& required) const {
    return (!required.bridge || bridge) && (!required.conference || conference) && (!required.record || record) && (!required.transcode || transcode);
  }
};

// Per-operation options, read from the SDP rather than the transport: a WebRTC offer needs ICE and DTLS whatever it arrived over.
struct Flags {
  bool ice = false;
  bool dtls = false;
  bool srtp = false;
  bool rtcp_mux = false;

  // False when the description did not parse. An unreadable description says nothing about its leg, unlike a plain RTP one.
  bool readable = false;

  // Index into Call::participants of the participant this offer or answer belongs to.
  std::size_t participant = 0;

  using Profile = media::Profile;

  // The profile the description the engine produces must have. Mirror leaves it to the engine.
  Profile target = Profile::Mirror;

  // Where the leg this description is produced for should send its media, when the caller knows better than the engine's
  // configuration (a leg on the local network reaches the node at its local address). Empty leaves it to the engine. An
  // engine that picks addresses from its own interfaces, as rtpengine does, may ignore it.
  std::string address;

  // The profile of the end that wrote this description. Nothing when it was unreadable.
  std::optional<Profile> stated() const;

  // A guess from the transport of the outgoing flow, for a leg that has not described itself: ws and wss mean WebRTC
  // (RFC 7118), anything else plain RTP, and no transport Mirror.
  static Profile profile_for_transport(const std::string& transport);

  // Reads ICE, DTLS, SRTP and rtcp-mux from the description, so a node knows what an offer needs before it answers.
  static Flags from_sdp(const std::string& sdp);
};

// The SDP to pass on, or why the engine declined.
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

// Anchors, bridges or mixes the media for a call. offer and answer name the participant they belong to; a
// conference-capable driver also takes join, leave and roster.
//
// Drivers register by URL scheme and are chosen by configuration: builtin:// or rtpengine://host:port.
//
// Every operation that may be a network round trip is async: the caller is on the Core strand and must not wait.
class MediaEngine : public plugins::Plugin {
 public:
  ~MediaEngine() override = default;

  std::string kind() const final { return plugins::kinds::media; }

  // Completion of an offer or answer.
  using MediaHandler = std::function<void(Result)>;

  // connect() is async. close() is synchronous and safe to call twice.
  virtual void connect(plugins::Executor on, plugins::StatusHandler handler) = 0;
  virtual void close() = 0;
  virtual bool is_connected() const = 0;

  // Local knowledge; no round trip.
  virtual Capabilities capabilities() const = 0;

  // Whether this driver can produce a description in the given profile. The node asks before re-offering a leg in it.
  virtual bool produces(Profile profile) const {
    (void)profile;
    return true;
  }

  // offer() takes one participant's offer and yields the SDP to send on; answer() does the same for the answer coming back.
  virtual void offer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) = 0;
  virtual void answer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) = 0;

  // Releases everything held for the call. Safe for a call the engine never saw.
  virtual void release(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) = 0;

  // What the engine holds for the call, as a JSON object, for the admin API. Optional fields: `idle_seconds` (how long all
  // of the call's media has been silent) and `legs`, one object per end per stream with cumulative "packets_in",
  // "bytes_in" (from that end), "packets_out" and "bytes_out" (to it), measured at the engine.
  virtual void query(plugins::Executor on, std::shared_ptr<Call> call, plugins::Handler<std::string> handler) = 0;

  // Packets relayed over the engine's lifetime. Optional: the default reports nothing.
  virtual std::optional<std::uint64_t> packets_relayed() const { return std::nullopt; }

  // Recording, when capabilities().record is set; the defaults decline. Where a recording goes is the engine's own configuration.
  virtual void start_recording(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) {
    (void)call;
    _complete(std::move(on), std::move(handler), plugins::Status::failure("this engine does not record"));
  }

  virtual void stop_recording(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) {
    (void)call;
    _complete(std::move(on), std::move(handler), plugins::Status::failure("this engine does not record"));
  }

  // Conference operations, when capabilities().conference is set; the defaults decline.
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
