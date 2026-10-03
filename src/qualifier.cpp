//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "qualifier.h"

#include <utility>

#include "channel.h"
#include "core.h"
#include "media/media_engine.h"
#include "util.h"

namespace athenasip {

namespace {

std::string key_of(const std::string& aor, const std::shared_ptr<types::SIPUri>& contact) { return aor + "|" + (contact ? contact->to_string() : ""); }

bool carries_sdp(const std::shared_ptr<SIPMessage>& message) {
  if (message->body.empty() || !message->header->contains("Content-Type")) return false;
  return Util::to_lower(message->header->headers_map["Content-Type"][0]->to_string()).rfind("application/sdp", 0) == 0;
}

}  // namespace

Qualifier::Qualifier(std::shared_ptr<loggers::Logger> logger, std::weak_ptr<Core> core)
    : _logger(std::make_shared<loggers::LoggerScoped>("qualify", std::move(logger))), _core(std::move(core)) {}

void Qualifier::watch(const std::string& aor, const std::shared_ptr<types::SIPUri>& contact, const std::string& flow_id, std::uint32_t interval,
                      std::uint32_t expires_seconds) {
  if (!contact || flow_id.empty() || interval == 0 || expires_seconds == 0) return forget(aor, contact);

  const auto key = key_of(aor, contact);
  auto& probe = _probes[key];

  if (probe.timer) probe.timer->cancel();

  // A refresh keeps what the client said and the conversation it is in. A binding that
  // moved to another flow is another client as far as either is concerned.
  if (probe.flow_id != flow_id) {
    probe.said.reset();
    probe.unanswered = 0;
    probe.call_id = Util::generate_random_string("", 24) + "@qualify";
    probe.from_tag = Util::generate_random_string("", 12);
  }

  probe.aor = aor;
  probe.contact = contact;
  probe.flow_id = flow_id;
  probe.interval = interval;
  probe.expires_at = std::time(nullptr) + expires_seconds;

  // At once, as Asterisk does on registration, so what the client takes is known before its
  // first call rather than an interval later.
  _schedule(key, std::chrono::milliseconds(0));
}

void Qualifier::forget(const std::string& aor, const std::shared_ptr<types::SIPUri>& contact) {
  const auto found = _probes.find(key_of(aor, contact));
  if (found == _probes.end()) return;

  if (found->second.timer) found->second.timer->cancel();
  _probes.erase(found);
}

std::optional<media::Profile> Qualifier::said(const std::string& flow_id) const {
  if (flow_id.empty()) return std::nullopt;

  for (const auto& [key, probe] : _probes) {
    if (probe.flow_id == flow_id && probe.said) return probe.said;
  }
  return std::nullopt;
}

std::vector<Qualifier::Probe> Qualifier::list() const {
  std::vector<Probe> out;
  out.reserve(_probes.size());
  for (const auto& [key, probe] : _probes) out.push_back(probe);
  return out;
}

void Qualifier::_schedule(const std::string& key, std::chrono::milliseconds delay) {
  auto core = _core.lock();
  if (!core) return;

  auto found = _probes.find(key);
  if (found == _probes.end()) return;

  auto timers = core->timer_source();
  if (!timers) return;

  // Weak: a timer an interval away must not be what keeps this alive.
  std::weak_ptr<Qualifier> weak_self = weak_from_this();
  found->second.timer = timers->schedule(delay, [weak_self, key]() {
    auto self = weak_self.lock();
    if (!self) return;

    auto core = self->_core.lock();
    if (!core) return;

    core->post([self, key]() { self->_probe(key); });
  });
}

void Qualifier::_probe(const std::string& key) {
  auto core = _core.lock();
  if (!core) return;

  auto found = _probes.find(key);
  if (found == _probes.end()) return;
  auto& probe = found->second;

  if (std::time(nullptr) >= probe.expires_at) {
    _logger->debug("Binding " + key + " has expired - no longer qualifying it");
    _probes.erase(found);
    return;
  }

  // The flow is the only way to the client, and the reason for asking. A client whose flow
  // has gone has gone with it, and registers again on a new one.
  auto channel = core->channel_find(probe.flow_id);
  if (!channel || !channel->_connection) {
    _logger->debug("The flow for " + key + " has closed - no longer qualifying it");
    _probes.erase(found);
    return;
  }

  const auto advertised = core->advertised_for(*channel);
  auto request = _options_for(probe, channel->_connection->transport_name(), advertised.host, advertised.port);

  std::weak_ptr<Qualifier> weak_self = weak_from_this();
  core->client_transaction_start(
      request, channel,
      [weak_self, key](std::shared_ptr<SIPMessage> response) {
        if (auto self = weak_self.lock()) self->_on_answer(key, response);
      },
      [weak_self, key]() {
        if (auto self = weak_self.lock()) self->_on_silence(key);
      });
}

void Qualifier::_on_answer(const std::string& key, const std::shared_ptr<SIPMessage>& response) {
  const int code = response->header->response_code;
  if (code < 200) return;

  auto found = _probes.find(key);
  if (found == _probes.end()) return;
  auto& probe = found->second;

  // Any final response says the client is there: a 405 or a 481 comes from something
  // listening as surely as a 200 does.
  probe.unanswered = 0;
  probe.answered_at = std::time(nullptr);

  // RFC 3261 11.2: the 200 "MAY" carry a session description, and when it does it is the
  // client saying what it takes. A reply without one says nothing about media, which is
  // not the same as saying something else, so what was said before stands.
  if (code < 300 && carries_sdp(response)) {
    if (const auto stated = media::Flags::from_sdp(response->body).stated()) {
      if (probe.said != stated)
        _logger->info(probe.aor + " on " + probe.flow_id + " says its media is " + (*stated == media::Profile::WebRtc ? "WebRTC" : "not WebRTC"));
      probe.said = stated;
    }
  }

  _schedule(key, std::chrono::seconds(probe.interval));
}

void Qualifier::_on_silence(const std::string& key) {
  auto found = _probes.find(key);
  if (found == _probes.end()) return;
  auto& probe = found->second;

  probe.unanswered++;
  _logger->info("No answer to OPTIONS from " + probe.aor + " (" + std::to_string(probe.unanswered) + " in a row)");

  _schedule(key, std::chrono::seconds(probe.interval));
}

std::shared_ptr<SIPMessage> Qualifier::_options_for(Probe& probe, const std::string& transport, const std::string& host, std::uint16_t port) const {
  // RFC 3261 8.1.1: what every request carries. The branch has the magic cookie (8.1.1.7);
  // Accept asks for the description 11.2 says the 200 may carry.
  const auto branch = std::string("z9hG4bK") + Util::generate_random_string("", 16);
  const auto aor = std::make_shared<types::SIPUri>(probe.aor);

  std::string raw = "OPTIONS " + probe.contact->to_string() + " SIP/2.0\r\n";
  raw += "Via: SIP/2.0/" + Util::to_upper(transport) + " " + host + ":" + std::to_string(port) + ";branch=" + branch + ";rport\r\n";
  raw += "Max-Forwards: 70\r\n";
  raw += "From: <sip:athenasip@" + aor->host + ">;tag=" + probe.from_tag + "\r\n";
  raw += "To: <" + probe.aor + ">\r\n";
  raw += "Call-ID: " + probe.call_id + "\r\n";
  raw += "CSeq: " + std::to_string(++probe.cseq) + " OPTIONS\r\n";
  raw += "Accept: application/sdp\r\n";
  raw += "Content-Length: 0\r\n";

  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>(raw);
  message->branch = branch;
  return message;
}

}  // namespace athenasip
