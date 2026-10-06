//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <atomic>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../types/url.h"
#include "bencode.h"
#include "media_engine.h"
#include "public_address.h"

namespace athenasip::media {

// The production engine: rtpengine, driven over its ng protocol. It handles ICE, DTLS and SRTP, so it is the engine for WebRTC.
//
//   media:
//     url: rtpengine://203.0.113.9:2223
//     rtpengine:
//       engines: ["203.0.113.10:2223"]
//       timeout_ms: 500
//       attempts: 3
//       ping_interval: 10
//       media_address: 203.0.113.9
//
// The URL's engine and those in engines are a pool. A new call goes to an engine chosen from its Call-ID among
// those answering, and the engine is recorded on the call (Call::media_engine) so that every later request for it,
// from any node, goes where its ports are. An engine that stops answering gets no new calls until it answers a
// ping again; a call already on it stays there, since its media cannot move.
//
// An ng message is a UDP datagram of "<cookie> <bencoded dictionary>", answered in the same shape. rtpengine caches
// its answer against the cookie, so retransmitting the same datagram is safe, and this driver does so on silence.
class RtpengineMediaEngine : public MediaEngine, public std::enable_shared_from_this<RtpengineMediaEngine> {
 public:
  RtpengineMediaEngine(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url);
  ~RtpengineMediaEngine() override;

  std::string name() const override;
  std::string version() const override;

  bool configure(const YAML::Node& own_root, const Config& system) override;

  void connect(plugins::Executor on, plugins::StatusHandler handler) override;
  void close() override;
  bool is_connected() const override;

  // Bridge, record and transcode. Not conference.
  Capabilities capabilities() const override;

  void offer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) override;
  void answer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) override;
  void release(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) override;
  void query(plugins::Executor on, std::shared_ptr<Call> call, plugins::Handler<std::string> handler) override;

  void start_recording(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) override;
  void stop_recording(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) override;

  // rtpengine's default UDP control port.
  static constexpr std::uint16_t kDefaultPort = 2223;

 private:
  // One engine of the pool. Touched only on _strand once connect() has begun.
  struct Engine {
    std::string host;
    std::uint16_t port = kDefaultPort;
    boost::asio::ip::udp::endpoint endpoint;
    bool resolved = false;
    bool up = false;

    // What Call::media_engine holds for a call on this engine.
    std::string name() const { return "rtpengine://" + host + ":" + std::to_string(port); }
  };

  // One outstanding request. The datagram is kept whole: a retransmission must carry the same cookie and bytes.
  struct Pending {
    std::string cookie;
    std::string datagram;
    std::size_t engine = 0;
    bool ping = false;
    std::function<void(std::optional<Bencode>)> handler;
    unsigned attempts_left = 0;
    std::shared_ptr<boost::asio::steady_timer> timer;
  };

  using Reply = std::function<void(std::optional<Bencode>)>;

  // A request about a call: the reply, the engine that gave it, or why there is none.
  struct Answered {
    std::optional<Bencode> reply;
    std::string engine;
    std::string error;
  };

  using CallReply = std::function<void(Answered)>;

  // _command builds the dictionary; _request sends it to one engine and hands back the reply, or nothing on failure.
  Bencode _command(const std::string& name, const std::shared_ptr<Call>& call) const;
  void _request(Bencode command, std::size_t engine, Reply reply);

  // Sends a command about a call to the engine recorded on it, or, for a call on none yet, to one chosen from its
  // Call-ID. A chosen engine that does not answer is marked down and the next is tried, so a new call costs one
  // timeout when an engine dies. Runs on _strand.
  void _call_request(Bencode command, std::string recorded, std::string call_id, CallReply reply);
  void _try_engines(std::shared_ptr<Bencode> command, std::string call_id, std::shared_ptr<std::vector<bool>> tried, CallReply reply);

  // The engine of that name, or of the call's Call-ID among those answering and not yet tried.
  std::optional<std::size_t> _engine_named(const std::string& name) const;
  std::optional<std::size_t> _choose(const std::string& call_id, const std::vector<bool>& tried) const;

  void _mark(std::size_t engine, bool up);

  // Pings every engine; schedules the next round.
  void _ping_all(std::function<void()> done);
  void _ping_schedule();

  // Everything below runs on _strand, the only thread that touches _pending.
  std::shared_ptr<boost::asio::ip::udp::socket> _current_socket() const;
  void _send(const std::shared_ptr<Pending>& pending);
  void _receive();
  void _on_receive(const boost::system::error_code& ec, std::size_t length);
  void _on_timeout(const std::string& cookie);
  void _fail_all(const std::string& reason);

  // The ng protocol names a leg by call id and dialog tag (RFC 3261 12.1.1).
  struct Tags {
    bool ok = false;
    std::string from;
    std::string to;
  };

  static Tags _tags_for(const std::shared_ptr<Call>& call, const Flags& flags, bool answering);

  // Empty when the reply is ok, otherwise rtpengine's reason.
  static std::string _error_of(const std::optional<Bencode>& reply);

  std::string _idle_document(const std::shared_ptr<Call>& call, const Bencode& reply) const;

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<types::URL> _url;

  std::vector<Engine> _engines;
  std::atomic<std::size_t> _up{0};
  std::string _media_address;

  // media_address, which may be a name (public_address.h).
  PublicAddress _public;

  // The address rtpengine is told to advertise: flags.address when set, otherwise media_address as it resolves now.
  std::string _advertise_for(const Flags& flags) const;

  // Per attempt. rtpengine answers in microseconds, so this bounds silence, and all attempts together must stay well
  // inside the 32 seconds of timer B.
  unsigned _timeout_ms = 500;
  unsigned _attempts = 3;

  // Seconds between pings of the pool.
  unsigned _ping_interval = 10;
  std::shared_ptr<boost::asio::steady_timer> _ping_timer;

  boost::asio::strand<boost::asio::io_context::executor_type> _strand;

  // Behind a mutex because close() is called off the strand. close() hands the socket to the strand to be closed.
  mutable std::mutex _socket_mutex;
  std::shared_ptr<boost::asio::ip::udp::socket> _socket;

  boost::asio::ip::udp::endpoint _received_from;

  // Sized for the largest UDP payload.
  std::vector<char> _receive_buffer;

  std::unordered_map<std::string, std::shared_ptr<Pending>> _pending;
};

}  // namespace athenasip::media
