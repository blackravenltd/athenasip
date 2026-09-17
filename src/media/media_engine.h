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
#include <unordered_map>
#include <utility>
#include <vector>

#include "../call.h"
#include "../loggers/logger.h"
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
class MediaEngine {
 public:
  virtual ~MediaEngine() = default;

  virtual std::string get_driver_name() const = 0;

  virtual bool connect() = 0;
  virtual void close() = 0;
  virtual bool is_connected() const = 0;

  virtual Capabilities capabilities() const = 0;

  // Two-way media setup. offer() takes the offer from one participant and returns the
  // SDP to send on; answer() takes the answer coming back.
  virtual Result offer(std::shared_ptr<Call> call, const std::string& sdp, const Flags& flags) = 0;
  virtual Result answer(std::shared_ptr<Call> call, const std::string& sdp, const Flags& flags) = 0;

  // Give up every resource held for this call. Safe to call for a call the engine
  // never saw.
  virtual bool release(std::shared_ptr<Call> call) = 0;

  // What the engine currently holds for this call, for the admin API and diagnostics.
  virtual std::string query(std::shared_ptr<Call> call) = 0;

  // Conference operations. Only meaningful when capabilities().conference is set; the
  // defaults below decline, so a bridge-only driver does not have to implement them.
  virtual bool join(std::shared_ptr<Call> call, std::size_t participant) {
    (void)call;
    (void)participant;
    return false;
  }

  virtual bool leave(std::shared_ptr<Call> call, std::size_t participant) {
    (void)call;
    (void)participant;
    return false;
  }

  virtual std::vector<std::string> roster(std::shared_ptr<Call> call) {
    (void)call;
    return {};
  }

  template <typename T, typename = std::enable_if_t<std::is_base_of_v<MediaEngine, T>>>
  static void register_driver(std::shared_ptr<loggers::Logger> logger, std::string scheme) {
    auto& drivers = get_drivers();
    logger->debug("(media_engine) Registering scheme " + scheme);

    drivers[std::move(scheme)] = [](std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url) -> std::shared_ptr<MediaEngine> {
      return std::static_pointer_cast<MediaEngine>(std::make_shared<T>(std::move(logger), std::move(url)));
    };
  }

  static std::shared_ptr<MediaEngine> create_driver(std::shared_ptr<loggers::Logger> logger, const std::string& url_string) {
    auto url = std::make_shared<types::URL>(url_string);

    logger->debug("(media_engine) Finding scheme " + url->scheme);

    auto& drivers = get_drivers();
    auto it = drivers.find(url->scheme);
    if (it == drivers.end()) {
      logger->error("(media_engine) Unknown scheme " + url->scheme);
      return nullptr;
    }

    return it->second(std::move(logger), std::move(url));
  }

 protected:
  using Factory = std::function<std::shared_ptr<MediaEngine>(std::shared_ptr<loggers::Logger>, std::shared_ptr<types::URL>)>;

  static std::unordered_map<std::string, Factory>& get_drivers() {
    static std::unordered_map<std::string, Factory> drivers;
    return drivers;
  }
};

}  // namespace athenasip::media
