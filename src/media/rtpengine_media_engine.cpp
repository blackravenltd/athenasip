//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "rtpengine_media_engine.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/ip/address.hpp>
#include <chrono>
#include <ctime>
#include <utility>

#include "../config.h"
#include "../global_io_context.h"
#include "../util.h"

namespace athenasip::media {

namespace {

constexpr std::size_t kMaxDatagram = 65535;

unsigned positive_or(const YAML::Node& node, unsigned fallback) {
  if (!node) return fallback;

  const auto value = node.as<int>(0);
  if (value <= 0) return fallback;

  return static_cast<unsigned>(value);
}

// RFC 8838, RFC 8842 and RFC 5764 through rtpengine's own vocabulary. What a leg needs
// is not what the leg on the other side sent, and with no flag at all rtpengine mirrors
// what it was handed: a WebRTC offer produces a WebRTC offer to the callee, which is
// right for browser to browser and exactly wrong for browser to desk phone.
//
// DTLS is passive towards a browser because the browser is the one that starts the
// handshake, and rtcp-mux is required there because a browser will not offer separate
// RTCP (RFC 5761, and what every implementation does).
void apply_profile(Bencode& command, Flags::Profile profile, bool source_wants_mux, bool answering) {
  switch (profile) {
    case Flags::Profile::WebRtc:
      command.set("ICE", Bencode(std::string("force")));
      // Only in an offer. Towards an offerer that said actpass the engine has already
      // chosen active and started the handshake as soon as ICE came up, which is its
      // right as the answerer (RFC 5763 section 5). Telling it passive in the answer
      // flips the role under a handshake in flight: the engine resets and waits, the
      // offerer was told passive too late to start one, and nothing ever arrives.
      if (!answering) command.set("DTLS", Bencode(std::string("passive")));
      command.set("transport-protocol", Bencode(std::string("UDP/TLS/RTP/SAVPF")));
      command.set("rtcp-mux", Bencode::list({Bencode(std::string("offer")), Bencode(std::string("require"))}));
      return;

    case Flags::Profile::SrtpSdes:
      // RFC 4568: the keys travel in the description, so there is no handshake and no
      // ICE. RTP/SAVP rather than the browser's UDP/TLS/RTP/SAVPF, and rtpengine
      // generates the crypto attributes from the profile alone.
      command.set("ICE", Bencode(std::string("remove")));
      command.set("DTLS", Bencode(std::string("off")));
      command.set("transport-protocol", Bencode(std::string("RTP/SAVP")));
      command.set("rtcp-mux", Bencode::list({Bencode(std::string("demux"))}));
      return;

    case Flags::Profile::PlainRtp:
      command.set("ICE", Bencode(std::string("remove")));
      command.set("DTLS", Bencode(std::string("off")));
      command.set("transport-protocol", Bencode(std::string("RTP/AVP")));
      command.set("rtcp-mux", Bencode::list({Bencode(std::string("demux"))}));
      return;

    case Flags::Profile::Mirror:
      // Nothing said, so rtpengine keeps what it was given. Offering mux onward only
      // where the end that sent this asked for it, because an endpoint that did not is
      // an endpoint that may not understand it.
      if (source_wants_mux) command.set("rtcp-mux", Bencode::list({Bencode(std::string("offer"))}));
      return;
  }
}

}  // namespace

RtpengineMediaEngine::RtpengineMediaEngine(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url)
    : _logger(std::make_shared<loggers::LoggerScoped>("rtpengine", std::move(logger))),
      _url(std::move(url)),
      _strand(boost::asio::make_strand(detail::get_global_io_context())),
      _receive_buffer(kMaxDatagram) {
  if (_url) {
    _host = _url->host;
    if (_url->port) _port = *_url->port;
  }
}

RtpengineMediaEngine::~RtpengineMediaEngine() { close(); }

std::string RtpengineMediaEngine::name() const { return "rtpengine"; }

std::string RtpengineMediaEngine::version() const { return "0.0.1"; }

bool RtpengineMediaEngine::configure(const YAML::Node& own_root, const Config& system) {
  (void)system;

  if (own_root && own_root.IsMap()) {
    _timeout_ms = positive_or(own_root["timeout_ms"], _timeout_ms);
    _attempts = positive_or(own_root["attempts"], _attempts);

    if (own_root["media_address"]) _media_address = own_root["media_address"].as<std::string>();
  }

  if (_host.empty()) {
    _logger->error("rtpengine:// needs a host: the engine is somewhere else by definition");
    return false;
  }

  return true;
}

Capabilities RtpengineMediaEngine::capabilities() const {
  Capabilities capabilities;
  capabilities.bridge = true;
  capabilities.record = true;
  capabilities.transcode = true;
  return capabilities;
}

bool RtpengineMediaEngine::is_connected() const { return _connected.load(std::memory_order_relaxed); }

void RtpengineMediaEngine::connect(plugins::Executor on, plugins::StatusHandler handler) {
  if (is_connected()) return _complete(std::move(on), std::move(handler), plugins::Status::success());

  if (_host.empty()) {
    return _complete(std::move(on), std::move(handler), plugins::Status::failure("no rtpengine host configured"));
  }

  auto self = shared_from_this();

  boost::asio::dispatch(_strand, [this, self, on, handler = std::move(handler)]() mutable {
    boost::system::error_code ec;
    auto address = boost::asio::ip::make_address(_host, ec);

    if (ec) {
      // A name rather than a literal. Resolving it here is a blocking call on the io
      // thread, which is acceptable exactly once at startup and nowhere else: the
      // engine's address is read before there is a call to hold up.
      try {
        boost::asio::ip::udp::resolver resolver(detail::get_global_io_context());
        auto endpoints = resolver.resolve(boost::asio::ip::udp::v4(), _host, std::to_string(_port));

        if (endpoints.empty()) {
          return _complete(std::move(on), std::move(handler), plugins::Status::failure("could not resolve " + _host));
        }

        _endpoint = *endpoints.begin();
      } catch (const std::exception& e) {
        return _complete(std::move(on), std::move(handler), plugins::Status::failure("could not resolve " + _host + " - " + e.what()));
      }
    } else {
      _endpoint = boost::asio::ip::udp::endpoint(address, _port);
    }

    auto socket = std::make_shared<boost::asio::ip::udp::socket>(detail::get_global_io_context());

    socket->open(_endpoint.protocol(), ec);
    if (ec) {
      return _complete(std::move(on), std::move(handler), plugins::Status::failure("could not open a socket - " + ec.message()));
    }

    {
      std::lock_guard<std::mutex> lock(_socket_mutex);
      _socket = std::move(socket);
    }

    _receive();

    // The engine is only connected once it has answered. A UDP socket opens whether or
    // not anything is listening, so without the ping a node would start, report a media
    // engine, and fail the first call instead of failing to start.
    _connected.store(true, std::memory_order_relaxed);

    auto ping = Bencode::dictionary({{"command", Bencode(std::string("ping"))}});

    _request(std::move(ping), [this, self, on, handler = std::move(handler)](std::optional<Bencode> reply) mutable {
      if (!reply) {
        close();
        return _complete(std::move(on), std::move(handler), plugins::Status::failure("no answer from rtpengine at " + _host + ":" + std::to_string(_port)));
      }

      const auto result = reply->string_at("result");

      if (result != "pong") {
        close();
        return _complete(std::move(on), std::move(handler), plugins::Status::failure("rtpengine answered a ping with \"" + result + "\""));
      }

      _logger->info("Connected to rtpengine at " + _host + ":" + std::to_string(_port));
      _complete(std::move(on), std::move(handler), plugins::Status::success());
    });
  });
}

void RtpengineMediaEngine::close() {
  _connected.store(false, std::memory_order_relaxed);

  std::shared_ptr<boost::asio::ip::udp::socket> socket;

  {
    std::lock_guard<std::mutex> lock(_socket_mutex);
    socket = std::move(_socket);
  }

  if (!socket) return;

  // The socket is closed on the strand and by a handler that owns it, so nothing
  // reaches into a member from the shutdown thread and nothing outlives what it
  // touches. Closing it ends the receive loop with operation_aborted, and that loop
  // holds the last reference this object has to itself.
  //
  // The pending requests are failed rather than dropped, or a caller waiting on one
  // waits for ever. That needs the object, so it is taken weakly: close() is also
  // called from the destructor, where there is nothing left to resurrect and by which
  // point there can be no pending requests, because each one holds a reference.
  auto weak = weak_from_this();

  boost::asio::post(_strand, [socket, weak]() {
    boost::system::error_code ec;
    socket->close(ec);

    if (auto self = weak.lock()) self->_fail_all("the media engine was closed");
  });

  _logger->info("Closed");
}

void RtpengineMediaEngine::_fail_all(const std::string& reason) {
  auto pending = std::move(_pending);
  _pending.clear();

  for (auto& [cookie, entry] : pending) {
    if (entry->timer) entry->timer->cancel();
    if (entry->handler) entry->handler(std::nullopt);
  }

  if (!pending.empty()) _logger->warn(std::to_string(pending.size()) + " request(s) abandoned: " + reason);
}

// The ng protocol's request identity. rtpengine caches its answer against the cookie,
// so a retransmission with the same one is answered from that cache rather than acted
// on twice - which is what makes it safe to send an offer again when the first went
// missing.
Bencode RtpengineMediaEngine::_command(const std::string& name, const std::shared_ptr<Call>& call) const {
  auto command = Bencode::dictionary({{"command", Bencode(name)}});
  if (call) command.set("call-id", Bencode(call->id));
  return command;
}

std::shared_ptr<boost::asio::ip::udp::socket> RtpengineMediaEngine::_current_socket() const {
  std::lock_guard<std::mutex> lock(_socket_mutex);
  return _socket;
}

void RtpengineMediaEngine::_request(Bencode command, Reply reply) {
  auto self = shared_from_this();

  boost::asio::dispatch(_strand, [this, self, command = std::move(command), reply = std::move(reply)]() mutable {
    auto socket = _current_socket();

    if (!socket || !socket->is_open()) {
      if (reply) reply(std::nullopt);
      return;
    }

    auto pending = std::make_shared<Pending>();
    pending->cookie = Util::generate_random_string("", 16);
    pending->datagram = pending->cookie + " " + command.encode();
    pending->handler = std::move(reply);
    pending->attempts_left = _attempts;
    pending->timer = std::make_shared<boost::asio::steady_timer>(_strand);

    _pending[pending->cookie] = pending;
    _send(pending);
  });
}

void RtpengineMediaEngine::_send(const std::shared_ptr<Pending>& pending) {
  auto socket = _current_socket();
  if (!socket || !socket->is_open() || pending->attempts_left == 0) return;

  --pending->attempts_left;

  // Synchronous, because a datagram to a socket with room does not block and this runs
  // on the strand where the pending table already lives. A send that cannot be made at
  // all reports rather than waits.
  boost::system::error_code ec;
  socket->send_to(boost::asio::buffer(pending->datagram), _endpoint, 0, ec);

  if (ec) {
    _logger->warn("Could not send to rtpengine - " + ec.message());

    // A send that failed outright will fail again on the next attempt for the same
    // reason, so the caller is told now rather than after the retransmissions.
    auto handler = std::move(pending->handler);
    _pending.erase(pending->cookie);
    if (handler) handler(std::nullopt);
    return;
  }

  auto self = shared_from_this();
  const auto cookie = pending->cookie;

  pending->timer->expires_after(std::chrono::milliseconds(_timeout_ms));
  pending->timer->async_wait(boost::asio::bind_executor(_strand, [this, self, cookie](const boost::system::error_code& error) {
    if (error) return;
    _on_timeout(cookie);
  }));
}

void RtpengineMediaEngine::_on_timeout(const std::string& cookie) {
  auto search = _pending.find(cookie);
  if (search == _pending.end()) return;

  auto pending = search->second;

  if (pending->attempts_left > 0) {
    _logger->warn("No answer from rtpengine in " + std::to_string(_timeout_ms) + "ms, asking again");
    return _send(pending);
  }

  auto handler = std::move(pending->handler);
  _pending.erase(search);

  if (handler) handler(std::nullopt);
}

// Every continuation carries self, so the buffer it is reading into cannot go away
// under it. That reference is also the only thing keeping this object alive once the
// caller has let go, which is why close() is what ends the loop: the socket closes, the
// receive completes with operation_aborted, the chain stops and the reference goes.
void RtpengineMediaEngine::_receive() {
  auto socket = _current_socket();
  if (!socket || !socket->is_open()) return;

  auto self = shared_from_this();

  socket->async_receive_from(
      boost::asio::buffer(_receive_buffer), _received_from,
      boost::asio::bind_executor(_strand, [this, self, socket](const boost::system::error_code& ec, std::size_t length) { _on_receive(ec, length); }));
}

void RtpengineMediaEngine::_on_receive(const boost::system::error_code& ec, std::size_t length) {
  if (ec) {
    // operation_aborted is close() doing its job; anything else is worth knowing about
    // and worth carrying on from, because the next datagram may be fine.
    if (ec != boost::asio::error::operation_aborted) {
      _logger->warn("Receive failed - " + ec.message());
      _receive();
    }
    return;
  }

  const std::string datagram(_receive_buffer.data(), length);
  const auto space = datagram.find(' ');

  if (space == std::string::npos) {
    _logger->warn("Datagram with no cookie, ignored");
    _receive();
    return;
  }

  const auto cookie = datagram.substr(0, space);
  auto search = _pending.find(cookie);

  if (search == _pending.end()) {
    // The answer to a request already given up on, or one this node never sent. Either
    // way there is nobody to hand it to.
    _receive();
    return;
  }

  auto pending = search->second;
  _pending.erase(search);

  if (pending->timer) pending->timer->cancel();

  auto reply = Bencode::decode(std::string_view(datagram).substr(space + 1));

  if (!reply) _logger->warn("Could not decode the answer from rtpengine");

  if (pending->handler) pending->handler(std::move(reply));

  _receive();
}

std::string RtpengineMediaEngine::_error_of(const std::optional<Bencode>& reply) {
  if (!reply) return "no answer from rtpengine";

  const auto result = reply->string_at("result");
  if (result == "ok" || result == "pong") return "";

  const auto reason = reply->string_at("error-reason");
  if (!reason.empty()) return reason;

  return result.empty() ? "rtpengine answered without a result" : "rtpengine answered \"" + result + "\"";
}

// RFC 3261 12.1.1: a dialog is the Call-ID and the two tags, and which of the two names
// the end whose description this is depends on which end sent it. rtpengine wants the
// offerer as from-tag throughout an exchange, so an answer names the offerer as from
// and the answerer as to - the reverse of the participant that handed us the SDP.
RtpengineMediaEngine::Tags RtpengineMediaEngine::_tags_for(const std::shared_ptr<Call>& call, const Flags& flags, bool answering) {
  Tags tags;

  if (!call || flags.participant >= call->participants.size()) return tags;

  const auto& participant = call->participants[flags.participant];
  const auto& dialog = participant.dialog;

  if (!dialog) return tags;

  const auto& own = participant.originator ? dialog->caller_tag : dialog->callee_tag;
  const auto& other = participant.originator ? dialog->callee_tag : dialog->caller_tag;

  if (answering) {
    // The offer came from the other end, so it is the one rtpengine has already filed
    // this media under.
    if (other.empty() || own.empty()) return tags;
    tags.from = other;
    tags.to = own;
  } else {
    if (own.empty()) return tags;
    tags.from = own;
    tags.to = other;
  }

  tags.ok = true;
  return tags;
}

void RtpengineMediaEngine::offer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) {
  if (!is_connected()) return _complete(std::move(on), std::move(handler), Result::failure("rtpengine is not connected"));

  const auto tags = _tags_for(call, flags, false);
  if (!tags.ok) return _complete(std::move(on), std::move(handler), Result::failure("no dialog tag for the offering participant"));

  auto command = _command("offer", call);
  command.set("from-tag", Bencode(tags.from));
  if (!tags.to.empty()) command.set("to-tag", Bencode(tags.to));
  command.set("sdp", Bencode(std::move(sdp)));

  // The substitutions this node would otherwise make itself, and the ones the builtin
  // relay does make. The o= line and the session-level c= name this node rather than
  // the endpoint, so a call whose media is anchored does not hand one end the other's
  // address (RFC 8866 section 5.2).
  //
  // sdp-version puts the version in rtpengine's hands too, which is RFC 3264 section
  // 8: what this node emits is not what the endpoint sent, so two offers the endpoint
  // considered identical can come out of here different, and passing its version
  // through would tell the far end nothing had changed.
  command.set("replace", Bencode::list({Bencode(std::string("origin")), Bencode(std::string("session-connection")), Bencode(std::string("sdp-version"))}));

  apply_profile(command, flags.target, flags.rtcp_mux, false);

  if (!_media_address.empty()) command.set("media-address", Bencode(_media_address));

  auto self = shared_from_this();

  _request(std::move(command), [this, self, on, handler = std::move(handler)](std::optional<Bencode> reply) mutable {
    const auto error = _error_of(reply);
    if (!error.empty()) return _complete(std::move(on), std::move(handler), Result::failure(error));

    auto sdp = reply->string_at("sdp");
    if (sdp.empty()) return _complete(std::move(on), std::move(handler), Result::failure("rtpengine answered ok with no sdp"));

    _complete(std::move(on), std::move(handler), Result::success(std::move(sdp)));
  });
}

void RtpengineMediaEngine::answer(plugins::Executor on, std::shared_ptr<Call> call, std::string sdp, Flags flags, MediaHandler handler) {
  if (!is_connected()) return _complete(std::move(on), std::move(handler), Result::failure("rtpengine is not connected"));

  const auto tags = _tags_for(call, flags, true);
  if (!tags.ok) return _complete(std::move(on), std::move(handler), Result::failure("no dialog tags for the answering participant"));

  auto command = _command("answer", call);
  command.set("from-tag", Bencode(tags.from));
  command.set("to-tag", Bencode(tags.to));
  command.set("sdp", Bencode(std::move(sdp)));
  command.set("replace", Bencode::list({Bencode(std::string("origin")), Bencode(std::string("session-connection")), Bencode(std::string("sdp-version"))}));

  apply_profile(command, flags.target, flags.rtcp_mux, true);
  if (!_media_address.empty()) command.set("media-address", Bencode(_media_address));

  auto self = shared_from_this();

  _request(std::move(command), [this, self, on, handler = std::move(handler)](std::optional<Bencode> reply) mutable {
    const auto error = _error_of(reply);
    if (!error.empty()) return _complete(std::move(on), std::move(handler), Result::failure(error));

    auto sdp = reply->string_at("sdp");
    if (sdp.empty()) return _complete(std::move(on), std::move(handler), Result::failure("rtpengine answered ok with no sdp"));

    _complete(std::move(on), std::move(handler), Result::success(std::move(sdp)));
  });
}

void RtpengineMediaEngine::release(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) {
  if (!call) return _complete(std::move(on), std::move(handler), plugins::Status::success());
  if (!is_connected()) return _complete(std::move(on), std::move(handler), plugins::Status::failure("rtpengine is not connected"));

  // No tags: a delete naming the call alone takes every leg of it, which is what
  // releasing a call means here. Releasing one leg would leave the other holding ports
  // for a call that has ended.
  auto command = _command("delete", call);

  auto self = shared_from_this();

  _request(std::move(command), [this, self, on, handler = std::move(handler)](std::optional<Bencode> reply) mutable {
    const auto error = _error_of(reply);
    if (error.empty()) return _complete(std::move(on), std::move(handler), plugins::Status::success());

    _complete(std::move(on), std::move(handler), plugins::Status::failure(error));
  });
}

void RtpengineMediaEngine::query(plugins::Executor on, std::shared_ptr<Call> call, plugins::Handler<std::string> handler) {
  if (!call) return _complete(std::move(on), std::move(handler), plugins::Result<std::string>::success("{}"));
  if (!is_connected()) return _complete(std::move(on), std::move(handler), plugins::Result<std::string>::failure("rtpengine is not connected"));

  auto command = _command("query", call);

  auto self = shared_from_this();

  _request(std::move(command), [this, self, call, on, handler = std::move(handler)](std::optional<Bencode> reply) mutable {
    const auto error = _error_of(reply);
    if (!error.empty()) return _complete(std::move(on), std::move(handler), plugins::Result<std::string>::failure(error));

    _complete(std::move(on), std::move(handler), plugins::Result<std::string>::success(_idle_document(call, *reply)));
  });
}

// What the call sweep reads, in the shape the builtin engine answers in: how long the
// quietest moment ago was across every stream, because one stream still carrying is a
// call still up.
//
// rtpengine reports a per-stream "last packet" as a unix timestamp, and zero for a
// stream nothing has arrived on. A stream that has never carried is idle from when the
// call was created rather than not idle at all, which is what the builtin relay reports
// too - its counter starts at the moment the relay does.
std::string RtpengineMediaEngine::_idle_document(const std::shared_ptr<Call>& call, const Bencode& reply) const {
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  const auto created = reply.integer_at("created", now);

  std::int64_t newest = 0;
  std::size_t streams = 0;

  // One per stream: rtpengine's "stats" is what arrived on the stream's port from its end,
  // and "stats_out", where a version reports it, what the engine sent that end.
  std::string legs = "[";

  if (const auto* tags = reply.find("tags"); tags != nullptr && tags->is_dictionary()) {
    for (const auto& [tag, leg] : tags->entries()) {
      const auto* medias = leg.find("medias");
      if (medias == nullptr || !medias->is_list()) continue;

      for (const auto& media : medias->values()) {
        const auto* leg_streams = media.find("streams");
        if (leg_streams == nullptr || !leg_streams->is_list()) continue;

        for (const auto& stream : leg_streams->values()) {
          ++streams;

          const auto last = stream.integer_at("last packet", 0);
          const auto seen = last > 0 ? last : created;

          if (seen > newest) newest = seen;

          if (legs.size() > 1) legs += ",";
          legs += "{";
          const auto* in = stream.find("stats");
          legs += "\"packets_in\":" + std::to_string(in && in->is_dictionary() ? in->integer_at("packets", 0) : 0);
          legs += ",\"bytes_in\":" + std::to_string(in && in->is_dictionary() ? in->integer_at("bytes", 0) : 0);
          if (const auto* out = stream.find("stats_out"); out && out->is_dictionary()) {
            legs += ",\"packets_out\":" + std::to_string(out->integer_at("packets", 0));
            legs += ",\"bytes_out\":" + std::to_string(out->integer_at("bytes", 0));
          }
          legs += "}";
        }
      }
    }
  }

  // A call rtpengine is holding no streams for has no media to be idle, and says so
  // with null rather than with a number the sweep would act on.
  const auto idle = streams == 0 ? std::string("null") : std::to_string(now > newest ? now - newest : 0);

  legs += "]";

  return "{\"call_id\":\"" + call->id + "\",\"engine\":\"rtpengine\",\"streams\":" + std::to_string(streams) + ",\"idle_seconds\":" + idle +
         ",\"legs\":" + legs + "}";
}

void RtpengineMediaEngine::start_recording(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) {
  if (!call) return _complete(std::move(on), std::move(handler), plugins::Status::failure("no call"));
  if (!is_connected()) return _complete(std::move(on), std::move(handler), plugins::Status::failure("rtpengine is not connected"));

  auto self = shared_from_this();

  _request(_command("start recording", call), [this, self, on, handler = std::move(handler)](std::optional<Bencode> reply) mutable {
    const auto error = _error_of(reply);
    if (error.empty()) return _complete(std::move(on), std::move(handler), plugins::Status::success());

    _complete(std::move(on), std::move(handler), plugins::Status::failure(error));
  });
}

void RtpengineMediaEngine::stop_recording(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) {
  if (!call) return _complete(std::move(on), std::move(handler), plugins::Status::failure("no call"));
  if (!is_connected()) return _complete(std::move(on), std::move(handler), plugins::Status::failure("rtpengine is not connected"));

  auto self = shared_from_this();

  _request(_command("stop recording", call), [this, self, on, handler = std::move(handler)](std::optional<Bencode> reply) mutable {
    const auto error = _error_of(reply);
    if (error.empty()) return _complete(std::move(on), std::move(handler), plugins::Status::success());

    _complete(std::move(on), std::move(handler), plugins::Status::failure(error));
  });
}

}  // namespace athenasip::media
