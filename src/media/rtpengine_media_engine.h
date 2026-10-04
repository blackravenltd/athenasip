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
//       timeout_ms: 500
//       attempts: 3
//       media_address: 203.0.113.9
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
  // One outstanding request. The datagram is kept whole: a retransmission must carry the same cookie and bytes.
  struct Pending {
    std::string cookie;
    std::string datagram;
    std::function<void(std::optional<Bencode>)> handler;
    unsigned attempts_left = 0;
    std::shared_ptr<boost::asio::steady_timer> timer;
  };

  using Reply = std::function<void(std::optional<Bencode>)>;

  // _command builds the dictionary; _request sends it and hands back the reply, or nothing on failure.
  Bencode _command(const std::string& name, const std::shared_ptr<Call>& call) const;
  void _request(Bencode command, Reply reply);

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

  std::string _host;
  std::uint16_t _port = kDefaultPort;
  std::string _media_address;

  // media_address, which may be a name (public_address.h).
  PublicAddress _public;

  // The address rtpengine is told to advertise: flags.address when set, otherwise media_address as it resolves now.
  std::string _advertise_for(const Flags& flags) const;

  // Per attempt. rtpengine answers in microseconds, so this bounds silence, and all attempts together must stay well
  // inside the 32 seconds of timer B.
  unsigned _timeout_ms = 500;
  unsigned _attempts = 3;

  boost::asio::strand<boost::asio::io_context::executor_type> _strand;

  // Behind a mutex because close() is called off the strand. close() hands the socket to the strand to be closed.
  mutable std::mutex _socket_mutex;
  std::shared_ptr<boost::asio::ip::udp::socket> _socket;

  boost::asio::ip::udp::endpoint _endpoint;
  boost::asio::ip::udp::endpoint _received_from;

  // Sized for the largest UDP payload.
  std::vector<char> _receive_buffer;

  std::unordered_map<std::string, std::shared_ptr<Pending>> _pending;

  std::atomic<bool> _connected{false};
};

}  // namespace athenasip::media
