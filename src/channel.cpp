//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "channel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/bind/bind.hpp>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "call.h"
#include "delayed_task.h"
#include "expiry_set.h"
#include "headers/authorization_header.h"
#include "headers/cseq_header.h"
#include "headers/header.h"
#include "headers/sip_identity_header.h"
#include "headers/string_header.h"
#include "headers/uint_header.h"
#include "headers/via_header.h"
#include "sdp.h"
#include "servers/connection.h"
#include "sip_header.h"
#include "sip_message.h"
#include "stun.h"
#include "types/authorization.h"
#include "types/sip_identity.h"
#include "types/sip_uri.h"
#include "types/subscriber.h"
#include "util.h"

using namespace athenasip::headers;
using namespace athenasip::types;
using namespace athenasip::loggers;
using namespace athenasip::servers;

namespace athenasip {

namespace {

// The message as sent, with Digest credentials and challenges redacted: a response is
// replayable while its nonce lives. The header line itself is kept.
std::string for_logging(const std::shared_ptr<SIPMessage>& message) {
  static const std::vector<std::string> secret = {"authorization:", "proxy-authorization:", "www-authenticate:", "proxy-authenticate:"};

  const auto wire = message->to_string();

  std::string out;
  out.reserve(wire.size());

  std::size_t at = 0;
  while (at < wire.size()) {
    auto end = wire.find("\r\n", at);
    if (end == std::string::npos) end = wire.size();

    const auto line = wire.substr(at, end - at);
    const auto lowered = Util::to_lower(line);

    bool redacted = false;
    for (const auto& name : secret) {
      if (lowered.rfind(name, 0) == 0) {
        out += line.substr(0, name.size() - 1) + ": <redacted>";
        redacted = true;
        break;
      }
    }

    if (!redacted) out += line;

    if (end == wire.size()) break;

    out += "\r\n";
    at = end + 2;
  }

  return out;
}

}  // namespace

Channel::Channel(std::shared_ptr<Logger> logger, std::shared_ptr<Core> core, std::shared_ptr<Connection> connection) : _connection(connection), _core(core) {
  _flow_id = Core::channel_key(_connection->transport_name(), _connection->remote_endpoint_name());
  _flow_token = _core->flow_tokens().seal(_flow_id);
  _logger = std::make_unique<LoggerScoped>("channel " + _flow_id, logger);
}

void Channel::start() {
  auto self(this->shared_from_this());

  // Servers call this from their own thread. dispatch runs inline when already on the strand.
  boost::asio::dispatch(_core->strand(), [this, self]() {
    _logger->info("Connected");
    state = State::Normal;

    _core->channel_register(_flow_id, self);

    _schedule_async_read();
  });
}

void Channel::close() {
  auto self(shared_from_this());

  boost::asio::dispatch(_core->strand(), [this, self]() {
    if (state == State::Closing || state == State::Closed) return;

    state = State::Closing;

    if (_connection) {
      // The socket belongs to its server's thread and is not thread-safe, so close it there.
      boost::asio::dispatch(_connection->executor(), [connection = _connection]() {
        if (connection->is_open()) {
          connection->shutdown();
          connection->close();
        }
      });

      _logger->info("Closed");

      _core->channel_unregister(_flow_id, self);

      _connection.reset();
    }

    state = State::Closed;
  });
}

void Channel::write(std::string message) {
  auto self(shared_from_this());

  boost::asio::dispatch(_core->strand(), [this, self, message = std::move(message)]() mutable { _schedule_async_write(std::move(message)); });
}

void Channel::send(std::shared_ptr<SIPMessage> message) {
  auto self(shared_from_this());

  boost::asio::dispatch(_core->strand(), [this, self, message]() { _send_on_strand(message); });
}

void Channel::_send_on_strand(std::shared_ptr<SIPMessage> message) {
  // A transaction can outlive the connection.
  if (!_connection) {
    _logger->info("Dropping " + message->header->summary() + " - channel is closed");
    return;
  }

  // The layers above own the Via (RFC 3261 16.6 step 8). The transport only ensures the top
  // one has a branch, which is what the response matches on (8.1.1.7).
  if (message->header->type == SIPHeader::Type::Request && message->header->contains("Via") && !message->header->headers_map["Via"].empty()) {
    auto via = message->header->headers_map["Via"][0]->as<ViaHeader>();

    if (via != nullptr && via->parameters["branch"].empty()) {
      if (message->branch.empty()) message->branch = Util::generate_random_string("z9hG4bK", 16);
      via->parameters["branch"] = message->branch;
    } else if (via != nullptr) {
      message->branch = via->parameters["branch"];
    }
  }

  message->header->clear("Content-Length");
  message->header->add("Content-Length", std::make_shared<UIntHeader>(message->body.size()));

  _logger->info("< " + message->header->summary());
  if (_core->config->sip_log_messages) _logger->debug("< " + for_logging(message));

  _schedule_async_write(message->to_string());
}

// On the strand.
void Channel::receive(std::shared_ptr<SIPMessage> message) {
  _logger->info("> " + message->header->summary());
  if (_core->config->sip_log_messages) _logger->debug("> " + for_logging(message));

  message->channel = shared_from_this();

  _stamp_via(message);

  _core->process_message(message);
}

// RFC 3261 18.2.1 and RFC 3581 section 4: record where a request really came from, so the
// response reaches a client behind NAT. `received` is the source address when it differs
// from the Via's; `rport` is the source port, when the client asked for it.
void Channel::_stamp_via(const std::shared_ptr<SIPMessage>& message) {
  if (!_connection) return;
  if (message->header->type != SIPHeader::Type::Request) return;
  if (!message->header->contains("Via") || message->header->headers_map["Via"].empty()) return;

  auto via = message->header->headers_map["Via"][0]->as<ViaHeader>();
  if (via == nullptr) return;

  const auto remote = _connection->remote_endpoint();
  const auto source = remote.address().to_string();

  // The Via host may carry a port, so compare on the address alone.
  const auto colon = via->host.rfind(':');
  const auto sent_by = colon == std::string::npos ? via->host : via->host.substr(0, colon);

  if (sent_by != source) via->parameters["received"] = source;

  // Only an empty rport is a request for the port.
  auto rport = via->parameters.find("rport");
  if (rport != via->parameters.end() && rport->second.empty()) rport->second = std::to_string(remote.port());
}

void Channel::_schedule_async_write(std::string message) {
  if (!_connection) return;

  // Sending keeps a flow alive as much as receiving does.
  touch();

  // The queue owns the buffer, which must outlive the write.
  _write_queue.push_back(std::make_shared<std::string>(std::move(message)));

  // A write is in flight; its completion picks this one up.
  if (_writing) return;

  _write_next();
}

void Channel::_write_next() {
  if (!_connection || _write_queue.empty()) {
    _writing = false;
    return;
  }

  _writing = true;

  auto self(shared_from_this());
  auto buffer = _write_queue.front();
  const auto offset = _write_offset;

  // The stream is only touched on the connection's executor.
  boost::asio::dispatch(_connection->executor(), [this, self, buffer, offset, connection = _connection]() {
    connection->async_write_some(boost::asio::buffer(buffer->data() + offset, buffer->size() - offset),
                                 [this, self, buffer](boost::system::error_code ec, std::size_t length) {
                                   // Back to the strand from the connection's thread.
                                   boost::asio::post(_core->strand(), [this, self, ec, length]() { _on_write(ec, length); });
                                 });
  });
}

void Channel::_on_write(boost::system::error_code ec, std::size_t length) {
  if (ec) {
    _logger->error("Write Error " + ec.to_string());

    _write_queue.clear();
    _write_offset = 0;
    _writing = false;

    close();
    return;
  }

  // async_write_some may write less than the whole buffer; resume from the offset.
  _write_offset += length;

  if (!_write_queue.empty() && _write_offset >= _write_queue.front()->size()) {
    _write_queue.pop_front();
    _write_offset = 0;
  }

  _write_next();
}

void Channel::_schedule_async_read() {
  auto self(shared_from_this());

  if (!_connection) return;

  // One read is outstanding at a time, so _read_buffer is not shared.
  boost::asio::dispatch(_connection->executor(), [this, self, connection = _connection]() {
    connection->async_read_some(boost::asio::buffer(_read_buffer), [this, self](boost::system::error_code ec, std::size_t length) {
      // Back to the strand from the connection's thread.
      boost::asio::post(_core->strand(), [this, self, ec, length]() { _on_read(ec, length); });
    });
  });
}

void Channel::touch() { _last_activity = _core->now(); }

namespace {

// An address of record reduced to what identifies it (RFC 3261 19.1.4): the user part
// exactly, the host lowercased, no parameters.
std::string subscriber_key(const std::string& aor) {
  const SIPUri uri(aor);
  return uri.user + "@" + Util::to_lower(uri.host);
}

}  // namespace

void Channel::authenticated_as(const std::string& aor) { _authenticated.insert(subscriber_key(aor)); }

bool Channel::is_authenticated_as(const std::string& aor) const { return _authenticated.count(subscriber_key(aor)) > 0; }

void Channel::_on_read(boost::system::error_code ec, std::size_t length) {
  auto self(shared_from_this());

  // Any bytes received keep the flow alive, whatever they turn out to be.
  if (!ec && length > 0) touch();

  {
    if (ec) {
      if (ec == boost::asio::error::operation_aborted) {
        // This node closed the connection.
      } else if (ec == boost::asio::error::eof || ec == boost::asio::ssl::error::stream_truncated) {
        // A TLS peer closing without close_notify (RFC 8446 6.1), as browsers and many
        // phones do, is a disconnect and not a fault.
        _logger->info("Remote Disconnected");
      } else {
        _logger->error("Read Error (" + ec.what() + ")");
      }
      close();
    } else {
      if (length == 0) {
        if (_connection) _schedule_async_read();
        return;
      };

      _buffer.append(_read_buffer.data(), length);

      // Nothing may throw out of the read handler: an exception would unwind through
      // io_context::run() and a malformed message would stop the node.
      try {
        _frame();
      } catch (const std::exception& e) {
        _logger->error(std::string("Dropping a message that could not be handled - ") + e.what());
        _buffer.clear();
        _incoming_message = nullptr;
      }

      if (_connection) _schedule_async_read();
    }
  }
}

void Channel::_frame() {
  if (_connection && !_connection->is_reliable()) return _frame_datagram();

  _frame_stream();
}

void Channel::_frame_stream() {
  // Still reading the body of a message whose headers have arrived.
  if (_incoming_message) {
    if (!_append_body()) return;

    receive(_incoming_message);
    _incoming_message = nullptr;
    return;
  }

  _take_keep_alives();

  // A read may hold several messages, or part of one.
  std::size_t split;

  while ((split = _buffer.find("\r\n\r\n")) != std::string::npos) {
    auto message = std::make_shared<SIPMessage>();
    message->channel = shared_from_this();
    message->source_port = _connection->remote_endpoint().port();
    message->header = std::make_shared<SIPHeader>(_buffer.substr(0, split));

    _buffer.erase(0, split + 4);

    // RFC 3261 18.3: Content-Length frames the body. Absent, it is zero.
    if (message->header->contains("Content-Length")) {
      auto length = message->header->headers_map["Content-Length"][0]->as<UIntHeader>();
      if (length != nullptr) message->body_length = length->value;
    }

    _incoming_message = message;
    _crlf_run = 0;

    if (!_append_body()) return;

    receive(_incoming_message);
    _incoming_message = nullptr;

    _take_keep_alives();
  }
}

// RFC 5626 4.4.1: a double CRLF between messages is a keep-alive ping, answered with a
// single CRLF. The ping may straddle two reads, so the count carries across them.
void Channel::_take_keep_alives() {
  while (_buffer.size() >= 2 && _buffer.compare(0, 2, "\r\n") == 0) {
    _buffer.erase(0, 2);

    if (++_crlf_run == 2) {
      _crlf_run = 0;

      // TCP and TLS only: a WebSocket has ping frames of its own (RFC 6455, RFC 7118).
      const auto transport = _connection ? Util::to_lower(_connection->transport_name()) : std::string();
      if (transport != "ws" && transport != "wss") _schedule_async_write("\r\n");
    }
  }
}

// A datagram is one whole message or is discarded; nothing carries over to the next.
void Channel::_frame_datagram() {
  std::string datagram;
  datagram.swap(_buffer);
  _incoming_message = nullptr;

  // RFC 5626 4.4.2: the UDP keep-alive is a STUN Binding request on the SIP port, answered
  // with the address it came from.
  if (stun::is_stun(datagram)) {
    if (!_connection) return;
    const auto from = _connection->remote_endpoint();
    if (auto response = stun::binding_response(datagram, from.address(), from.port())) _schedule_async_write(std::move(*response));
    return;
  }

  // A datagram of CRLFs is a keep-alive (RFC 5626 4.4.1).
  std::size_t at = 0;
  while (datagram.size() - at >= 2 && datagram.compare(at, 2, "\r\n") == 0) at += 2;
  if (at >= datagram.size()) return;

  const auto split = datagram.find("\r\n\r\n", at);

  if (split == std::string::npos) {
    _logger->warn("Datagram with no blank line after its headers, discarded");
    return;
  }

  auto message = std::make_shared<SIPMessage>();
  message->channel = shared_from_this();
  message->source_port = _connection->remote_endpoint().port();
  message->header = std::make_shared<SIPHeader>(datagram.substr(at, split - at));

  std::size_t declared = 0;

  if (message->header->contains("Content-Length")) {
    auto length = message->header->headers_map["Content-Length"][0]->as<UIntHeader>();
    if (length != nullptr) declared = length->value;
  }

  const auto carried = datagram.size() - (split + 4);

  // RFC 3261 18.3: a Content-Length greater than the body carried discards the message.
  if (declared > carried) {
    _logger->warn("Datagram claims " + std::to_string(declared) + " bytes of body and carries " + std::to_string(carried) + ", discarded");
    return;
  }

  // RFC 3261 18.3: a shorter Content-Length truncates the body to it.
  message->body = datagram.substr(split + 4, declared);
  message->body_length = static_cast<unsigned int>(declared);

  receive(message);
}

bool Channel::_append_body() {
  auto to_append = std::min(_incoming_message->body_length - _incoming_message->body.size(), _buffer.size());
  _incoming_message->body += _buffer.substr(0, to_append);
  _buffer.erase(0, to_append);
  // True once the body is complete.
  return (_incoming_message->body.size() == _incoming_message->body_length);
}

}  // namespace athenasip