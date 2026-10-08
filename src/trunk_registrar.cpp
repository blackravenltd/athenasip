//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "trunk_registrar.h"

#include <algorithm>
#include <boost/asio/ip/address.hpp>
#include <boost/json.hpp>
#include <utility>
#include <vector>

#include "channel.h"
#include "core.h"
#include "digest.h"
#include "events/topics.h"
#include "headers/sip_identity_header.h"
#include "headers/uint_header.h"
#include "util.h"

namespace athenasip {

namespace {

// Where a trunk's URI says to send: RFC 3263's defaults for what it leaves out.
dns::Hop hop_of(const types::SIPUri& uri) {
  const bool secure = Util::to_lower(uri.scheme) == "sips";
  dns::Hop hop;
  hop.transport = uri.has_parameter("transport") ? Util::to_lower(uri.parameter("transport")) : (secure ? "tls" : "udp");
  hop.address = uri.host;
  if (hop.address.size() > 2 && hop.address.front() == '[' && hop.address.back() == ']') hop.address = hop.address.substr(1, hop.address.size() - 2);
  hop.port = uri.port.value_or(hop.transport == "tls" ? 5061 : 5060);
  return hop;
}

bool is_literal(const std::string& host) {
  boost::system::error_code error;
  boost::asio::ip::make_address(host, error);
  return !error;
}

// RFC 3261 10.3 step 8: the lifetime the registrar granted this contact, from its expires parameter, else the
// Expires header. Nothing when the 2xx says neither.
std::optional<std::uint32_t> granted(const SIPMessage& response, const std::string& contact_user, const std::string& host, std::uint16_t port) {
  if (response.header->contains("Contact")) {
    for (const auto& value : response.header->headers_map.at("Contact")) {
      auto contact = value->as<headers::SIPIdentityHeader>();
      if (contact == nullptr || contact->value == nullptr || contact->value->uri == nullptr) continue;

      const auto& uri = *contact->value->uri;
      if (uri.user != contact_user || Util::to_lower(uri.host) != Util::to_lower(host) || uri.port.value_or(5060) != port) continue;

      const auto expires = contact->value->tags.find("expires");
      if (expires == contact->value->tags.end()) continue;
      try {
        return static_cast<std::uint32_t>(std::stoul(expires->second));
      } catch (const std::exception&) {
        continue;
      }
    }
  }

  if (response.header->contains("Expires")) {
    if (auto expires = response.header->headers_map.at("Expires")[0]->as<headers::UIntHeader>()) return static_cast<std::uint32_t>(expires->value);
  }
  return std::nullopt;
}

bool stale(const types::Authorization& challenge) {
  const auto found = challenge.fields.find("stale");
  return found != challenge.fields.end() && Util::to_lower(found->second) == "true";
}

}  // namespace

void TrunkStatuses::observe(const std::string& topic, const std::string& message) {
  // trunks/<name>/status
  const auto first = topic.find('/');
  const auto last = topic.rfind('/');
  if (first == std::string::npos || last == first) return;
  const auto name = topic.substr(first + 1, last - first - 1);

  boost::system::error_code error;
  auto parsed = boost::json::parse(message, error);

  std::lock_guard<std::mutex> lock(_mutex);
  if (error || !parsed.is_object()) {
    _reports.erase(name);
    return;
  }
  _reports[name] = parsed.as_object();
}

std::optional<boost::json::object> TrunkStatuses::find(const std::string& name) const {
  std::lock_guard<std::mutex> lock(_mutex);
  const auto found = _reports.find(types::Trunk::normalise(name));
  if (found == _reports.end()) return std::nullopt;
  return found->second;
}

TrunkRegistrar::TrunkRegistrar(std::shared_ptr<loggers::Logger> logger, std::weak_ptr<Core> core)
    : _logger(std::make_shared<loggers::LoggerScoped>("trunks", std::move(logger))), _core(std::move(core)) {}

void TrunkRegistrar::start() {
  _running = true;
  scan();
}

void TrunkRegistrar::stop() {
  _running = false;
  if (_scan_timer) _scan_timer->cancel();
  for (auto& [name, registration] : _registrations) {
    if (registration.timer) registration.timer->cancel();
  }
}

std::map<std::string, TrunkRegistrar::Status> TrunkRegistrar::statuses() const {
  std::map<std::string, Status> out;
  for (const auto& [name, registration] : _registrations) out[registration.trunk.name] = registration.status;
  return out;
}

void TrunkRegistrar::_schedule_scan() {
  auto core = _core.lock();
  if (!core || !_running) return;

  std::weak_ptr<TrunkRegistrar> weak_self = weak_from_this();
  _scan_timer = core->timer_source()->schedule(kScan, [weak_self]() {
    auto self = weak_self.lock();
    if (!self) return;
    if (auto core = self->_core.lock()) core->post([self]() { self->scan(); });
  });
}

void TrunkRegistrar::scan() {
  auto core = _core.lock();
  if (!core) return;

  auto self = shared_from_this();
  core->datastore->trunk_list(core->strand(), [this, self](plugins::Result<std::vector<std::shared_ptr<types::Trunk>>> found) {
    auto core = _core.lock();
    if (!core) return;

    _schedule_scan();

    if (!found.ok) {
      // A datastore that holds no trunks has none to register.
      if (found.error.find("does not support") == std::string::npos) _logger->error("Cannot read the trunks - " + found.error);
      return;
    }

    std::vector<std::string> wanted;
    for (const auto& trunk : found.value) {
      if (!trunk->register_enabled) continue;
      wanted.push_back(trunk->key());

      // One node per trunk: the carrier would see the Contact change at every refresh otherwise.
      core->datastore->lease(core->strand(), "trunk-register:" + trunk->key(), core->config->sip_node_id, kLeaseSeconds,
                             [this, self, trunk](plugins::Result<bool> held) {
                               // A store with no leases serves one node, which registers.
                               if (!held.ok || held.value) return _keep(*trunk);
                               _drop(trunk->key(), false);
                             });
    }

    std::vector<std::string> gone;
    for (const auto& [key, registration] : _registrations) {
      if (std::find(wanted.begin(), wanted.end(), key) == wanted.end()) gone.push_back(key);
    }
    for (const auto& key : gone) _drop(key, true);
  });
}

void TrunkRegistrar::_keep(const types::Trunk& trunk) {
  auto core = _core.lock();
  if (!core) return;

  const auto key = trunk.key();
  auto found = _registrations.find(key);

  if (found != _registrations.end()) {
    auto& registration = found->second;

    // Asked to register again while its binding was being removed: it registers after all.
    if (registration.leaving) {
      registration.leaving = false;
      registration.trunk = trunk;
      registration.expires = trunk.register_expires;
      if (registration.timer) registration.timer->cancel();
      return _register(key);
    }

    const auto& was = registration.trunk;
    const bool changed = was.uri != trunk.uri || was.proxy != trunk.proxy || was.username != trunk.username || was.password != trunk.password ||
                         was.contact_user != trunk.contact_user || was.register_expires != trunk.register_expires || was.tls_ca != trunk.tls_ca;
    registration.trunk = trunk;
    if (!changed) return;

    _logger->info("Trunk " + trunk.name + " changed - registering again");
    registration.expires = trunk.register_expires;
    registration.failures = 0;
    if (registration.timer) registration.timer->cancel();
    return _register(key);
  }

  Registration registration;
  registration.trunk = trunk;
  registration.call_id = Util::generate_random_string("", 24) + "@" + core->config->sip_node_id;
  registration.from_tag = Util::generate_random_string("", 12);
  registration.expires = trunk.register_expires;
  registration.status.state = "registering";
  _registrations.emplace(key, std::move(registration));

  _logger->info("Registering to trunk " + trunk.name + " at " + trunk.uri);
  _register(key);
}

void TrunkRegistrar::_drop(const std::string& name, bool unregister) {
  auto found = _registrations.find(name);
  if (found == _registrations.end()) return;
  auto& registration = found->second;
  if (registration.timer) registration.timer->cancel();

  if (unregister && registration.status.state == "registered") {
    if (registration.leaving) return;
    _logger->info("Trunk " + registration.trunk.name + " no longer registers - removing its binding at the carrier");
    registration.leaving = true;
    registration.expires = 0;
    registration.failures = 0;
    return _register(name);
  }

  _gone(name);
}

void TrunkRegistrar::_gone(const std::string& name) {
  auto found = _registrations.find(name);
  if (found == _registrations.end()) return;

  if (found->second.timer) found->second.timer->cancel();
  _logger->info("No longer registering to trunk " + found->second.trunk.name);

  // A retained status for a trunk nobody registers would be shown for ever; an empty one clears it.
  if (found->second.leaving) {
    if (auto core = _core.lock(); core && core->events) core->events->publish_state(events::topics::trunk_status(found->second.trunk.key()), "");
  }
  _registrations.erase(found);
}

void TrunkRegistrar::_register(const std::string& name) {
  auto core = _core.lock();
  if (!core) return;

  auto found = _registrations.find(name);
  if (found == _registrations.end() || found->second.in_flight) return;

  // Sent to the outbound proxy when there is one, else where the trunk's URI says.
  const auto& sent_to = found->second.trunk.proxy.empty() ? found->second.trunk.uri : found->second.trunk.proxy;
  const types::SIPUri uri(sent_to);
  if (!uri.valid || uri.host.empty()) return _failed(name, "its URI " + sent_to + " does not parse");

  found->second.in_flight = true;
  const auto tls_ca = found->second.trunk.tls_ca;
  auto self = shared_from_this();

  const auto connect = [this, self, name, tls_ca](const dns::Hop& hop) {
    auto core = _core.lock();
    if (!core) return;

    core->channel_connect(
        hop.transport, hop.address, hop.port,
        [this, self, name, hop](plugins::Result<std::shared_ptr<Channel>> opened) {
          auto found = _registrations.find(name);
          if (found == _registrations.end()) return;

          if (!opened.ok || !opened.value || !opened.value->_connection) {
            found->second.in_flight = false;
            return _failed(name, "no flow to " + hop.transport + "://" + hop.address + ":" + std::to_string(hop.port) + " - " + opened.error);
          }
          _send(name, opened.value, std::nullopt, false);
        },
        hop.transport == "tls" ? std::optional<std::string>(tls_ca) : std::nullopt);
  };

  const auto hop = hop_of(uri);
  if (is_literal(hop.address)) return connect(hop);

  // RFC 3263 for a name: the first hop it gives.
  core->locator()->locate(core->strand(), uri, [this, self, name, connect](plugins::Result<std::vector<dns::Hop>> located) {
    auto found = _registrations.find(name);
    if (found == _registrations.end()) return;

    if (!located.ok || located.value.empty()) {
      found->second.in_flight = false;
      return _failed(name, "DNS gives nowhere to send it - " + (located.ok ? std::string("no SIP service") : located.error));
    }
    connect(located.value.front());
  });
}

void TrunkRegistrar::_send(const std::string& name, const std::shared_ptr<Channel>& channel, const std::optional<types::Authorization>& credentials,
                           bool proxy_auth) {
  auto core = _core.lock();
  if (!core) return;

  auto found = _registrations.find(name);
  if (found == _registrations.end()) return;
  auto& registration = found->second;
  const auto& trunk = registration.trunk;

  // RFC 3261 10.2: the Request-URI is the registrar's domain, without a user; To and From are the address of record.
  types::SIPUri registrar(trunk.uri);
  registrar.user.clear();
  const auto user = trunk.username.empty() ? trunk.contact_user : trunk.username;
  const auto contact_user = trunk.contact_user.empty() ? user : trunk.contact_user;
  const auto scheme = Util::to_lower(registrar.scheme) == "sips" ? std::string("sips") : std::string("sip");
  const auto aor = scheme + ":" + user + "@" + registrar.host;

  const auto transport = Util::to_lower(channel->_connection->transport_name());
  const auto advertised = core->advertised_for(*channel);
  const auto branch = std::string("z9hG4bK") + Util::generate_random_string("", 16);

  std::string raw = "REGISTER " + registrar.to_string() + " SIP/2.0\r\n";
  raw += "Via: SIP/2.0/" + Util::to_upper(transport) + " " + advertised.host + ":" + std::to_string(advertised.port) + ";branch=" + branch + ";rport\r\n";
  raw += "Max-Forwards: 70\r\n";
  raw += "From: <" + aor + ">;tag=" + registration.from_tag + "\r\n";
  raw += "To: <" + aor + ">\r\n";
  raw += "Call-ID: " + registration.call_id + "\r\n";
  raw += "CSeq: " + std::to_string(++registration.cseq) + " REGISTER\r\n";
  raw += "Contact: <sip:" + contact_user + "@" + advertised.host + ":" + std::to_string(advertised.port) +
         (transport == "udp" ? "" : ";transport=" + transport) + ">\r\n";
  raw += "Expires: " + std::to_string(registration.expires) + "\r\n";
  if (credentials) raw += std::string(proxy_auth ? "Proxy-Authorization" : "Authorization") + ": " + credentials->to_string() + "\r\n";
  raw += "Content-Length: 0\r\n";

  auto request = std::make_shared<SIPMessage>();
  request->header = std::make_shared<SIPHeader>(raw);
  request->branch = branch;

  const bool answering = credentials.has_value();
  std::weak_ptr<TrunkRegistrar> weak_self = weak_from_this();
  core->client_transaction_start(
      request, channel,
      [weak_self, name, channel, request, answering](std::shared_ptr<SIPMessage> response) {
        if (auto self = weak_self.lock()) self->_on_answer(name, channel, request, response, answering);
      },
      [weak_self, name]() {
        auto self = weak_self.lock();
        if (!self) return;
        if (auto found = self->_registrations.find(name); found != self->_registrations.end()) found->second.in_flight = false;
        self->_failed(name, "no answer");
      });
}

void TrunkRegistrar::_on_answer(const std::string& name, const std::shared_ptr<Channel>& channel, const std::shared_ptr<SIPMessage>& request,
                                const std::shared_ptr<SIPMessage>& response, bool answered_challenge) {
  const int code = response->header->response_code;
  if (code < 200) return;

  auto found = _registrations.find(name);
  if (found == _registrations.end()) return;
  auto& registration = found->second;
  registration.in_flight = false;
  const auto& trunk = registration.trunk;

  // RFC 3261 22.2: the trunk's credentials, once; again only for a nonce that went stale.
  if (code == 401 || code == 407) {
    const std::string asking = code == 407 ? "Proxy-Authenticate" : "WWW-Authenticate";
    std::vector<types::Authorization> challenges;
    if (response->header->contains(asking)) {
      for (const auto& value : response->header->headers_map[asking]) challenges.emplace_back(value->to_string());
    }

    const auto challenge = digest::preferred(challenges);
    if (answered_challenge && !(challenge && stale(*challenge)))
      return _failed(name, "the carrier refused the trunk's credentials (" + std::to_string(code) + ")");
    if (trunk.username.empty()) return _failed(name, "the carrier asks for credentials and the trunk has no username");

    const auto answer = challenge ? digest::respond(*challenge, trunk.username, trunk.password, "REGISTER", request->header->request_uri->to_string(),
                                                    Util::generate_random_string("", 16), 1)
                                  : std::nullopt;
    if (!answer) return _failed(name, "the carrier's challenge is not one this node can answer");

    registration.in_flight = true;
    return _send(name, channel, answer, code == 407);
  }

  // RFC 3261 10.3 step 7: too brief, with the shortest it accepts.
  if (code == 423 && response->header->contains("Min-Expires")) {
    if (auto minimum = response->header->headers_map["Min-Expires"][0]->as<headers::UIntHeader>();
        minimum != nullptr && minimum->value > registration.expires) {
      _logger->info("Trunk " + trunk.name + " wants at least " + std::to_string(minimum->value) + " seconds");
      registration.expires = static_cast<std::uint32_t>(minimum->value);
      registration.in_flight = true;
      return _send(name, channel, std::nullopt, false);
    }
  }

  if (registration.leaving) {
    if (code < 200 || code >= 300) _logger->info("Trunk " + trunk.name + " did not remove its binding (" + std::to_string(code) + "); it lapses");
    return _gone(name);
  }

  if (code < 200 || code >= 300) return _failed(name, std::to_string(code) + " " + response->header->response_message);

  auto core = _core.lock();
  if (!core) return;

  const auto advertised = core->advertised_for(*channel);
  const auto contact_user = trunk.contact_user.empty() ? trunk.username : trunk.contact_user;
  const auto lifetime = granted(*response, contact_user, advertised.host, advertised.port).value_or(registration.expires);
  if (lifetime == 0) return _failed(name, "the carrier granted no time");

  const bool first = registration.status.state != "registered";
  registration.failures = 0;
  registration.status = Status{"registered", std::to_string(code) + " " + response->header->response_message, std::time(nullptr) + lifetime};
  if (first) _logger->info("Registered to trunk " + trunk.name + " for " + std::to_string(lifetime) + " seconds");
  _publish(name);

  // Refreshed a minute early, or at three quarters for a short one, so a lost REGISTER has time to be resent.
  _after(name, std::chrono::seconds(lifetime > 120 ? lifetime - 60 : std::max<std::uint32_t>(lifetime * 3 / 4, 1)));
}

void TrunkRegistrar::_failed(const std::string& name, const std::string& why) {
  auto found = _registrations.find(name);
  if (found == _registrations.end()) return;
  auto& registration = found->second;

  // Removing a binding is tried once: if it cannot be, the binding lapses at the carrier.
  if (registration.leaving) return _gone(name);

  ++registration.failures;
  registration.status = Status{"failed", why, 0};
  _logger->warn("Cannot register to trunk " + registration.trunk.name + " - " + why);
  _publish(name);

  // 30 seconds, doubling, to ten minutes.
  const auto shift = std::min<std::uint32_t>(registration.failures - 1, 5);
  _after(name, std::chrono::seconds(std::min<std::uint32_t>(30u << shift, 600)));
}

void TrunkRegistrar::_after(const std::string& name, std::chrono::seconds delay) {
  auto core = _core.lock();
  if (!core) return;

  auto found = _registrations.find(name);
  if (found == _registrations.end()) return;

  if (found->second.timer) found->second.timer->cancel();

  std::weak_ptr<TrunkRegistrar> weak_self = weak_from_this();
  found->second.timer = core->timer_source()->schedule(delay, [weak_self, name]() {
    auto self = weak_self.lock();
    if (!self) return;
    if (auto core = self->_core.lock()) core->post([self, name]() { self->_register(name); });
  });
}

void TrunkRegistrar::_publish(const std::string& name) {
  auto core = _core.lock();
  if (!core || !core->events) return;

  auto found = _registrations.find(name);
  if (found == _registrations.end()) return;

  const auto& status = found->second.status;
  boost::json::object report;
  report["trunk"] = found->second.trunk.name;
  report["node"] = core->config->sip_node_id;
  report["state"] = status.state;
  report["detail"] = status.detail;
  report["expires_at"] = static_cast<std::int64_t>(status.expires_at);
  report["at"] = Util::get_zulu_time();
  core->events->publish_state(events::topics::trunk_status(found->second.trunk.key()), boost::json::serialize(report));
}

}  // namespace athenasip
