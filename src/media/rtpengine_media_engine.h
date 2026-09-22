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

namespace athenasip::media {

// The production media engine: rtpengine, spoken to over its ng protocol. This is the
// engine that does WebRTC, because ICE, DTLS and SRTP are what a browser requires and
// what the builtin relay declines rather than pretends to.
//
//   media:
//     url: rtpengine://203.0.113.9:2223
//     rtpengine:
//       timeout_ms: 500
//       attempts: 3
//       media_address: 203.0.113.9
//
// The ng protocol is a UDP datagram of "<cookie> <bencoded dictionary>", answered with
// a datagram of the same shape. The cookie is the request identity and rtpengine caches
// its answer against it, which is what makes a retransmission safe: the same cookie
// asks for the answer again rather than for the work again. That is the whole of the
// reliability story over UDP, and it is why this driver retransmits rather than giving
// up on the first silence.
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

  // Bridge, record and transcode. Not conference: rtpengine's publish/subscribe is a
  // later item and claiming it now would have a caller ask for a mix and get a relay.
  Capabilities capabilities() const override;

  void offer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) override;
  void answer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) override;
  void release(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) override;
  void query(plugins::Executor on, std::shared_ptr<Call> call, plugins::Handler<std::string> handler) override;

  void start_recording(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) override;
  void stop_recording(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) override;

  // The default ng port. rtpengine's own is 2223 for the UDP control socket.
  static constexpr std::uint16_t kDefaultPort = 2223;

 private:
  // What one outstanding request holds. The datagram is kept whole rather than rebuilt,
  // because a retransmission has to carry the same cookie and the same bytes or it is a
  // second request rather than the same one again.
  struct Pending {
    std::string cookie;
    std::string datagram;
    std::function<void(std::optional<Bencode>)> handler;
    unsigned attempts_left = 0;
    std::shared_ptr<boost::asio::steady_timer> timer;
  };

  using Reply = std::function<void(std::optional<Bencode>)>;

  // The two halves of every operation: build the dictionary, then send it and turn what
  // comes back into the contract's answer.
  Bencode _command(const std::string& name, const std::shared_ptr<Call>& call) const;
  void _request(Bencode command, Reply reply);

  // Strand-confined. Everything below runs on _strand, which is the only thread that
  // touches the pending table.
  std::shared_ptr<boost::asio::ip::udp::socket> _current_socket() const;
  void _send(const std::shared_ptr<Pending>& pending);
  void _receive();
  void _on_receive(const boost::system::error_code& ec, std::size_t length);
  void _on_timeout(const std::string& cookie);
  void _fail_all(const std::string& reason);

  // The tags that name a leg to rtpengine. The ng protocol addresses media by call id
  // plus the tag of the end whose description is being passed in, which is the From tag
  // of the dialog for one end and the To tag for the other (RFC 3261 12.1.1).
  struct Tags {
    bool ok = false;
    std::string from;
    std::string to;
  };

  static Tags _tags_for(const std::shared_ptr<Call>& call, const Flags& flags, bool answering);

  // rtpengine answers every command with a result, and says why when it is not ok.
  static std::string _error_of(const std::optional<Bencode>& reply);

  std::string _idle_document(const std::shared_ptr<Call>& call, const Bencode& reply) const;

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<types::URL> _url;

  std::string _host;
  std::uint16_t _port = kDefaultPort;
  std::string _media_address;

  // Per attempt, not for the whole operation. rtpengine answers in microseconds when it
  // is there at all, so this is a bound on silence rather than on work; three attempts
  // at half a second is well inside the thirty-two seconds timer B gives a transaction
  // that a fork has to share.
  unsigned _timeout_ms = 500;
  unsigned _attempts = 3;

  boost::asio::strand<boost::asio::io_context::executor_type> _strand;

  // Held by shared_ptr and behind a mutex because close() is called from the shutdown
  // path rather than from the strand, and a socket is not safe for two threads. The
  // teardown hands the socket to the strand to be closed rather than reaching into it,
  // which is the same rule the transport follows for a connection's stream.
  mutable std::mutex _socket_mutex;
  std::shared_ptr<boost::asio::ip::udp::socket> _socket;

  boost::asio::ip::udp::endpoint _endpoint;
  boost::asio::ip::udp::endpoint _received_from;

  // One datagram can carry a whole session description twice over, so this is sized for
  // the largest a UDP payload can be rather than for the largest seen.
  std::vector<char> _receive_buffer;

  std::unordered_map<std::string, std::shared_ptr<Pending>> _pending;

  std::atomic<bool> _connected{false};
};

}  // namespace athenasip::media
