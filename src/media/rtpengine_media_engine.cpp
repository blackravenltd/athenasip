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

// Maps a profile onto rtpengine's flags (RFC 8838, RFC 8842, RFC 5764). With no flags rtpengine mirrors what it was
// handed, which is wrong when the two legs differ. Towards a browser DTLS is passive, because the browser starts the
// handshake, and rtcp-mux is required (RFC 5761).
void apply_profile(Bencode& command, Flags::Profile profile, bool source_wants_mux, bool answering) {
  switch (profile) {
    case Flags::Profile::WebRtc:
      command.set("ICE", Bencode(std::string("force")));
      // Offer only. As answerer to an actpass offer the engine has already gone active (RFC 5763 section 5); setting
      // passive in the answer would reset a handshake in flight and it would never complete.
      if (!answering) command.set("DTLS", Bencode(std::string("passive")));
      command.set("transport-protocol", Bencode(std::string("UDP/TLS/RTP/SAVPF")));
      command.set("rtcp-mux", Bencode::list({Bencode(std::string("offer")), Bencode(std::string("require"))}));
      return;

    case Flags::Profile::SrtpSdes:
      // RFC 4568: keys in the description, so no handshake and no ICE. rtpengine generates the crypto attributes.
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
      // rtpengine keeps what it was given. rtcp-mux is offered onward only if the sending end asked for it.
      if (source_wants_mux) command.set("rtcp-mux", Bencode::list({Bencode(std::string("offer"))}));
      return;
  }
}

}  // namespace

RtpengineMediaEngine::RtpengineMediaEngine(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url)
    : _logger(std::make_shared<loggers::LoggerScoped>("rtpengine", std::move(logger))),
      _url(std::move(url)),
      _strand(boost::asio::make_strand(detail::get_global_io_context())),
      _receive_buffer(kMaxDatagram),
      _public(_logger) {
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

  // Resolves a name before the node serves anything.
  _public.set(_media_address);
  _public.start();

  if (_host.empty()) {
    return _complete(std::move(on), std::move(handler), plugins::Status::failure("no rtpengine host configured"));
  }

  auto self = shared_from_this();

  boost::asio::dispatch(_strand, [this, self, on, handler = std::move(handler)]() mutable {
    boost::system::error_code ec;
    auto address = boost::asio::ip::make_address(_host, ec);

    if (ec) {
      // A name: resolved with a blocking call, acceptable only here at startup, before there is a call to hold up.
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

    // A UDP socket opens whether or not anything listens, so connecting succeeds only once rtpengine answers a ping;
    // close() clears the flag if it does not.
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

std::string RtpengineMediaEngine::_advertise_for(const Flags& flags) const { return flags.address.empty() ? _public.current() : flags.address; }

void RtpengineMediaEngine::close() {
  _connected.store(false, std::memory_order_relaxed);
  _public.stop();

  std::shared_ptr<boost::asio::ip::udp::socket> socket;

  {
    std::lock_guard<std::mutex> lock(_socket_mutex);
    socket = std::move(_socket);
  }

  if (!socket) return;

  // The socket is closed on the strand, which ends the receive loop with operation_aborted and drops the loop's
  // reference to this object. Pending requests are failed so no caller waits for ever; the object is held weakly
  // because close() also runs from the destructor.
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

  // Synchronous: a datagram send does not block, and this is already on the strand.
  boost::system::error_code ec;
  socket->send_to(boost::asio::buffer(pending->datagram), _endpoint, 0, ec);

  if (ec) {
    _logger->warn("Could not send to rtpengine - " + ec.message());

    // A send that failed outright would fail again, so report now rather than after the retransmissions.
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

// Each continuation holds self, keeping the buffer and this object alive. close() ends the loop: the receive
// completes with operation_aborted and the reference goes.
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
    // operation_aborted is close(); after anything else, carry on, since the next datagram may be fine.
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
    // A reply to a request already given up on, or never sent.
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

// RFC 3261 12.1.1. rtpengine wants the offerer as from-tag throughout an exchange, so an answer names the offerer
// as from and the answerer as to.
RtpengineMediaEngine::Tags RtpengineMediaEngine::_tags_for(const std::shared_ptr<Call>& call, const Flags& flags, bool answering) {
  Tags tags;

  if (!call || flags.participant >= call->participants.size()) return tags;

  const auto& participant = call->participants[flags.participant];
  const auto& dialog = participant.dialog;

  if (!dialog) return tags;

  const auto& own = participant.originator ? dialog->caller_tag : dialog->callee_tag;
  const auto& other = participant.originator ? dialog->callee_tag : dialog->caller_tag;

  if (answering) {
    // The offerer is the other end.
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

  // o= and the session-level c= name this node, so an anchored call does not give one end the other's address
  // (RFC 8866 section 5.2). sdp-version has rtpengine version what it emits (RFC 3264 section 8).
  command.set("replace", Bencode::list({Bencode(std::string("origin")), Bencode(std::string("session-connection")), Bencode(std::string("sdp-version"))}));

  apply_profile(command, flags.target, flags.rtcp_mux, false);

  if (const auto advertise = _advertise_for(flags); !advertise.empty()) command.set("media-address", Bencode(advertise));

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
  if (const auto advertise = _advertise_for(flags); !advertise.empty()) command.set("media-address", Bencode(advertise));

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

  // No tags: a delete naming only the call releases every leg of it.
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

// The query document, in the builtin engine's shape. idle_seconds is the time since the most recent packet on any
// stream: one stream still carrying is a call still up. rtpengine reports "last packet" per stream as a unix
// timestamp, zero if nothing has arrived; such a stream counts as idle since the call was created.
std::string RtpengineMediaEngine::_idle_document(const std::shared_ptr<Call>& call, const Bencode& reply) const {
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  const auto created = reply.integer_at("created", now);

  std::int64_t newest = 0;
  std::size_t streams = 0;

  // Per stream: "stats" is what arrived from the stream's end and "stats_out", where rtpengine reports it, what was sent to it.
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

  // A call with no streams reports null, not a number the call sweep would act on.
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
