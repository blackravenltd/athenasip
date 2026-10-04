//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "core.h"

#include <algorithm>
#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/json.hpp>
#include <chrono>
#include <optional>
#include <unordered_set>

#include "channel.h"
#include "events/topics.h"
#include "expiry_set.h"
#include "proxy.h"
#include "push/push_parameters.h"
#include "qualifier.h"
#include "registrar.h"
#include "rtp/rtp_relay.h"
#include "servers/tcp_connection.h"
#include "servers/tls_connection.h"
#include "servers/tls_context.h"
#include "transactions/invite_client_transaction.h"
#include "transactions/invite_server_transaction.h"
#include "transactions/non_invite_client_transaction.h"
#include "transactions/non_invite_server_transaction.h"
#include "types/sip_uri.h"
#include "util.h"

using namespace athenasip::servers;
using namespace athenasip::datastores;
using namespace athenasip::events;
using namespace athenasip::rtp;
using namespace athenasip::types;
using namespace athenasip::transactions;

namespace athenasip {

namespace {

// idle_seconds from a media engine's query() document: how long the call's relays have been
// silent. nullopt when the engine does not say.
std::optional<std::uint32_t> idle_seconds_of(const std::string& document) {
  try {
    const auto parsed = boost::json::parse(document);
    if (!parsed.is_object()) return std::nullopt;

    const auto* value = parsed.as_object().if_contains("idle_seconds");
    if (value == nullptr || !value->is_int64()) return std::nullopt;

    const auto seconds = value->as_int64();
    if (seconds < 0) return std::nullopt;

    return static_cast<std::uint32_t>(seconds);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace

Core::Core(std::shared_ptr<Logger> logger, std::shared_ptr<Config> _config, std::shared_ptr<athenasip::datastores::Datastore> _datastore,
           std::shared_ptr<events::EventSystem> _events)
    : _logger(std::make_unique<LoggerScoped>("core", logger)),
      config(_config),
      datastore(_datastore),
      events(_events),
      _strand(boost::asio::make_strand(detail::get_global_io_context())),
      _nonce_cache(std::make_shared<ExpirySet<std::string>>()) {}

void Core::server_register(std::shared_ptr<servers::Server> server) { _servers.push_back(server); }

void Core::server_start_all() {
  for (auto& server : _servers) server->start();
}

void Core::server_stop_all() {
  for (auto& server : _servers) server->stop();
}

void Core::realm_get_by_name(std::string realm_name, plugins::Handler<std::shared_ptr<Realm>> handler) {
  datastore->realm_get_by_name(_strand, std::move(realm_name), std::move(handler));
}

void Core::subscriber_get(std::shared_ptr<SIPIdentity> identity, plugins::Handler<std::shared_ptr<Subscriber>> handler) {
  datastore->subscriber_get(_strand, std::move(identity), std::move(handler));
}

void Core::location_list(std::uint64_t subscriber_id, plugins::Handler<std::vector<types::Location>> handler) {
  datastore->location_list(_strand, subscriber_id, std::move(handler));
}

void Core::subscriber_register(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel,
                               std::uint32_t expires_seconds, std::string path, plugins::StatusHandler handler, std::string instance, std::uint32_t reg_id,
                               bool push) {
  // RFC 3261 10.3 step 7: every successful REGISTER writes the binding. It records the
  // flow and the node holding it, the only way back to a client whose Contact is
  // unreachable (RFC 5626).
  types::Location binding;
  binding.contact = contact;
  binding.path = std::move(path);
  binding.node_id = config->sip_node_id;
  binding.instance = std::move(instance);
  binding.reg_id = reg_id;
  binding.push = push;
  if (channel) binding.flow_id = channel->flow_id();

  // The event waits for the write.
  datastore->subscriber_register(_strand, subscriber, std::move(binding), expires_seconds,
                                 [this, subscriber, contact, channel, handler](plugins::Status status) mutable {
                                   if (!status.ok) {
                                     _logger->error("Cannot register subscriber " + subscriber->identity->to_string() + " - " + status.error);
                                     if (handler) handler(status);
                                     return;
                                   }

                                   events->publish(events::topics::subscriber_status(subscriber->identity->uri->to_string()),
                                                   "{\"contact\":\"" + push::without_push_parameters(*contact).to_string() + "\",\"node\":\"" +
                                                       config->sip_node_id + "\",\"registered\":\"" + Util::get_zulu_time() + "\"}");

                                   if (handler) handler(status);
                                 });
}

void Core::subscriber_unregister(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel,
                                 plugins::StatusHandler handler) {
  (void)channel;

  auto self = shared_from_this();

  datastore->subscriber_unregister(_strand, subscriber, contact, [this, self, subscriber, handler](plugins::Status status) mutable {
    // Removing a binding that is not there is not an error (RFC 3261 10.3 step 7); the store
    // reports it with this message. Anything else is the store failing.
    if (!status.ok && status.error == "subscriber_unregister failed") {
      _logger->debug("No binding to remove for " + subscriber->identity->to_string());
    } else if (!status.ok) {
      _logger->error("Cannot unregister subscriber " + subscriber->identity->to_string() + " - " + status.error);
    }
    if (handler) handler(status);
  });
}

std::string Core::channel_key(const std::string& transport, const std::string& host, std::uint16_t port) {
  return channel_key(transport, host + ":" + std::to_string(port));
}

std::string Core::channel_key(const std::string& transport, const std::string& endpoint) { return Util::to_lower(transport) + "://" + endpoint; }

bool Core::channel_register(std::string endpoint, std::shared_ptr<Channel> channel) {
  _channels[endpoint] = channel;
  _channels_by_token[channel->flow_token()] = channel;

  // Every address a Record-Route this node writes can carry must be recognised when it
  // comes back as a Route (RFC 3261 16.4), or the node forwards the request to itself:
  // the local endpoint, the address advertised on this flow, and the public address.
  if (channel->_connection) {
    local_address_add(channel->_connection->local_endpoint_name());

    const auto local = channel->_connection->local_endpoint();
    const auto advertised = advertised_for(*channel);
    local_address_add(advertised.host + ":" + std::to_string(advertised.port));

    if (!config->sip_public_address.empty()) {
      const auto public_port = config->public_port_for(Util::to_lower(channel->_connection->transport_name()));
      local_address_add(config->sip_public_address + ":" + std::to_string(public_port != 0 ? public_port : local.port()));
    }
  }

  events->publish(events::topics::node_channel(config->sip_node_id, channel->_connection->transport_name(), channel->_connection->remote_endpoint_name()),
                  "{\"status\":\"registered\",\"at\":\"" + Util::get_zulu_time() + "\"}");

  _logger->debug("Registered Channel " + endpoint);
  return true;
}

void Core::channel_alias(std::string endpoint, const std::shared_ptr<Channel>& channel) {
  _channels[endpoint] = channel;
  _logger->debug("Aliased Channel " + endpoint);
}

bool Core::channel_unregister(std::string endpoint, std::shared_ptr<Channel> channel) {
  events->publish(events::topics::node_channel(config->sip_node_id, channel->_connection->transport_name(), channel->_connection->remote_endpoint_name()),
                  "{\"status\":\"closed\",\"at\":\"" + Util::get_zulu_time() + "\"}");

  // Remove every name the channel is filed under, aliases included.
  const auto removed = std::erase_if(_channels, [&channel](const auto& entry) { return entry.second == channel; });
  _channels_by_token.erase(channel->flow_token());

  _logger->debug("Unregistered Channel " + endpoint + (removed > 1 ? " and " + std::to_string(removed - 1) + " alias(es)" : ""));
  return true;
}

std::shared_ptr<Channel> Core::channel_find(const std::string& transport, const std::string& host, std::uint16_t port) {
  return channel_find(channel_key(transport, host, port));
}

std::shared_ptr<Channel> Core::channel_for_token(const std::string& token) {
  if (token.empty()) return nullptr;

  auto search = _channels_by_token.find(token);
  if (search == _channels_by_token.end()) return nullptr;

  return search->second;
}

std::shared_ptr<Channel> Core::channel_find(const std::string& flow_id) {
  if (flow_id.empty()) return nullptr;

  auto search = _channels.find(flow_id);
  if (search == _channels.end()) return nullptr;
  return search->second;
}

// RFC 3261 16.6 step 7 and 18.1. Outbound sockets run on the global io_context; the answer
// is posted back to the strand.
void Core::channel_connect(std::string transport, std::string host, std::uint16_t port, plugins::Handler<std::shared_ptr<Channel>> handler) {
  using ChannelResult = plugins::Result<std::shared_ptr<Channel>>;

  transport = Util::to_lower(transport);

  if (!config->sip_allow_unencrypted && transport != "tls" && transport != "wss") {
    return handler(ChannelResult::failure("sip.allow_unencrypted is false: no outbound " + transport + " flow"));
  }

  const auto key = channel_key(transport, host, port);

  if (auto existing = channel_find(transport, host, port)) return handler(ChannelResult::success(existing));

  if (transport == "udp") return _connect_datagram(host, port, std::move(handler));

  // Outbound TLS is only to cluster peers, with the cluster's certificates.
  if (transport == "tls" && !_cluster_tls) {
    return handler(ChannelResult::failure("cannot open an outbound tls flow without the cluster's certificates"));
  }

  if (transport != "tcp" && transport != "tls") {
    return handler(ChannelResult::failure("cannot open an outbound " + transport + " flow"));
  }

  const bool secure = transport == "tls";
  auto cluster_tls = _cluster_tls;

  auto& io_context = detail::get_global_io_context();

  auto resolver = std::make_shared<boost::asio::ip::tcp::resolver>(io_context);
  auto socket = std::make_shared<boost::asio::ip::tcp::socket>(io_context);
  auto deadline = std::make_shared<boost::asio::steady_timer>(io_context);

  // The deadline and the connect race; the handler is called exactly once.
  auto answered = std::make_shared<std::atomic<bool>>(false);

  std::weak_ptr<Core> weak_self = weak_from_this();

  auto answer = [weak_self, answered, socket, deadline, handler](ChannelResult result) {
    if (answered->exchange(true)) return;

    deadline->cancel();

    auto self = weak_self.lock();
    if (!self) return;

    if (!result.ok) {
      boost::system::error_code ec;
      socket->close(ec);
    }

    boost::asio::post(self->_strand, [handler, result = std::move(result)]() mutable { handler(std::move(result)); });
  };

  deadline->expires_after(std::chrono::milliseconds(config->sip_connect_timeout_ms));
  deadline->async_wait([answer, key](const boost::system::error_code& ec) {
    if (ec == boost::asio::error::operation_aborted) return;
    answer(ChannelResult::failure("timed out opening a flow to " + key));
  });

  // Address records only. RFC 3263 server selection is the caller's, through locator().
  resolver->async_resolve(
      host, std::to_string(port), [weak_self, resolver, socket, answer, key, secure, cluster_tls, host](const boost::system::error_code& ec, auto results) {
        if (ec) return answer(ChannelResult::failure("cannot resolve " + key + " - " + ec.message()));

        boost::asio::async_connect(*socket, results, [weak_self, socket, answer, key, secure, cluster_tls, host](const boost::system::error_code& ec, auto) {
          if (ec) return answer(ChannelResult::failure("cannot reach " + key + " - " + ec.message()));

          auto self = weak_self.lock();
          if (!self) return;

          if (secure) return self->_secure_flow(socket, cluster_tls, host, key, answer);

          std::shared_ptr<servers::Connection> connection = std::make_shared<servers::TCPConnection>(socket);
          if (!connection->start()) return answer(ChannelResult::failure("cannot start the flow to " + key));

          auto channel = std::make_shared<Channel>(self->_logger->base_logger(), self, connection);

          // start() files the channel under the address it reached; the alias adds the name
          // it was dialled by.
          channel->start();

          boost::asio::post(self->_strand, [self, channel, key]() { self->channel_alias(key, channel); });

          self->_logger->info("Opened flow to " + key + " as " + connection->remote_endpoint_name());
          answer(ChannelResult::success(channel));
        });
      });
}

bool Core::cluster_tls_set(const std::string& ca, const std::string& cert, const std::string& key) {
  auto context = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_client);
  if (!servers::load_tls_certificates(_logger, *context, cert, key) || !servers::require_peer_certificates(_logger, *context, ca)) return false;

  _cluster_tls = std::move(context);
  return true;
}

// The client half of the cluster's mutual TLS. The peer's certificate is checked against
// the cluster CA and the name dialled, so one member cannot answer for another.
void Core::_secure_flow(std::shared_ptr<boost::asio::ip::tcp::socket> socket, std::shared_ptr<boost::asio::ssl::context> context, const std::string& host,
                        const std::string& key, std::function<void(plugins::Result<std::shared_ptr<Channel>>)> answer) {
  using ChannelResult = plugins::Result<std::shared_ptr<Channel>>;

  auto stream = std::make_shared<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(std::move(*socket), *context);
  stream->set_verify_callback(boost::asio::ssl::host_name_verification(host));

  std::weak_ptr<Core> weak_self = weak_from_this();
  stream->async_handshake(boost::asio::ssl::stream_base::client, [weak_self, stream, key, answer](const boost::system::error_code& ec) {
    if (ec) {
      boost::system::error_code ignored;
      stream->lowest_layer().close(ignored);
      return answer(ChannelResult::failure("TLS to " + key + " failed - " + ec.message()));
    }

    auto self = weak_self.lock();
    if (!self) return;

    auto connection = std::make_shared<servers::TLSConnection>(stream, true);
    auto channel = std::make_shared<Channel>(self->_logger->base_logger(), self, connection);
    channel->start();

    boost::asio::post(self->_strand, [self, channel, key]() { self->channel_alias(key, channel); });

    self->_logger->info("Opened TLS flow to " + key + ", node " + connection->peer_identity());
    answer(ChannelResult::success(channel));
  });
}

// A new UDP flow leaves by a listener's socket, so its source port is the one the far end
// answers to and a NAT already maps (RFC 3261 18.1.1, RFC 3581).
void Core::_connect_datagram(std::string host, std::uint16_t port, plugins::Handler<std::shared_ptr<Channel>> handler) {
  using ChannelResult = plugins::Result<std::shared_ptr<Channel>>;

  const auto key = channel_key("udp", host, port);

  // A literal address needs no resolver.
  boost::system::error_code literal;
  const auto address = boost::asio::ip::make_address(host, literal);
  if (!literal) return _open_datagram({boost::asio::ip::udp::endpoint(address, port)}, key, std::move(handler));

  auto& io_context = detail::get_global_io_context();
  auto resolver = std::make_shared<boost::asio::ip::udp::resolver>(io_context);
  auto deadline = std::make_shared<boost::asio::steady_timer>(io_context);

  // The resolver is bounded by sip_connect_timeout_ms. The handler is called exactly once,
  // on the strand.
  auto answered = std::make_shared<std::atomic<bool>>(false);
  auto answer = [answered, deadline, handler](ChannelResult result) {
    if (answered->exchange(true)) return;

    deadline->cancel();
    handler(std::move(result));
  };

  std::weak_ptr<Core> weak_self = weak_from_this();

  deadline->expires_after(std::chrono::milliseconds(config->sip_connect_timeout_ms));
  deadline->async_wait([weak_self, answer, key](const boost::system::error_code& ec) {
    if (ec == boost::asio::error::operation_aborted) return;
    if (auto self = weak_self.lock()) boost::asio::post(self->_strand, [answer, key]() { answer(ChannelResult::failure("timed out resolving " + key)); });
  });

  resolver->async_resolve(host, std::to_string(port), [weak_self, resolver, answer, key](const boost::system::error_code& ec, auto results) {
    auto self = weak_self.lock();
    if (!self) return;

    std::vector<boost::asio::ip::udp::endpoint> candidates;
    if (!ec) {
      for (const auto& entry : results) candidates.push_back(entry.endpoint());
    }

    boost::asio::post(self->_strand, [self, candidates, answer, key, ec]() {
      if (candidates.empty()) return answer(ChannelResult::failure("cannot resolve " + key + (ec ? " - " + ec.message() : "")));
      self->_open_datagram(candidates, key, answer);
    });
  });
}

// On the strand.
void Core::_open_datagram(std::vector<boost::asio::ip::udp::endpoint> candidates, std::string key, plugins::Handler<std::shared_ptr<Channel>> handler) {
  using ChannelResult = plugins::Result<std::shared_ptr<Channel>>;

  auto self = shared_from_this();

  for (const auto& remote : candidates) {
    for (const auto& server : _servers) {
      const bool asked = server->open_datagram_flow(remote, [self, remote, key, handler](std::shared_ptr<servers::Connection> connection) {
        if (!connection) {
          // A datagram from the peer created the flow first. Use its channel if it has
          // registered; otherwise fail and let the next request find it.
          if (auto existing = self->channel_find("udp", remote.address().to_string(), remote.port())) return handler(ChannelResult::success(existing));
          return handler(ChannelResult::failure("a flow to " + key + " was being made by a datagram from it"));
        }

        auto channel = std::make_shared<Channel>(self->_logger->base_logger(), self, connection);

        // Already on the strand, so start() registers the channel inline.
        channel->start();
        if (channel->flow_id() != key) self->channel_alias(key, channel);

        self->_logger->info("Opened flow to " + key + " as " + connection->remote_endpoint_name());
        handler(ChannelResult::success(channel));
      });

      if (asked) return;
    }
  }

  handler(ChannelResult::failure("no UDP listener can send to " + key));
}

std::shared_ptr<dns::SipLocator> Core::locator() {
  if (!_locator) {
    auto servers = dns::UdpResolver::servers_from("/etc/resolv.conf");
    if (servers.empty()) _logger->warn("No nameservers in /etc/resolv.conf - SIP URIs naming a host will not resolve");

    _locator = std::make_shared<dns::SipLocator>(std::make_shared<dns::UdpResolver>(_logger->base_logger(), std::move(servers)));
  }
  return _locator;
}

void Core::local_address_add(std::string host_port) { _local_addresses.insert(std::move(host_port)); }

bool Core::is_local_address(const std::string& host, std::uint16_t port) const { return _local_addresses.count(host + ":" + std::to_string(port)) > 0; }

void Core::channel_close_all() {
  // close() erases from _channels, so walk a copy.
  std::vector<std::shared_ptr<Channel>> channels;
  channels.reserve(_channels.size());
  for (const auto& [endpoint, channel] : _channels) channels.push_back(channel);
  _channels.clear();
  _channels_by_token.clear();

  for (const auto& channel : channels) channel->close();
}

void Core::nonce_create(std::shared_ptr<Realm> realm, plugins::Handler<std::string> handler) {
  std::array<unsigned char, 16> random_bytes;

  if (RAND_bytes(random_bytes.data(), random_bytes.size()) != 1) {
    throw std::runtime_error("Failed to generate secure random bytes");
  }

  const uint64_t timestamp = static_cast<uint64_t>(std::time(nullptr));

  const std::string random_hex = Util::to_hex(random_bytes.data(), random_bytes.size());

  // The nonce is realm:random:timestamp, signed with the realm's secret.
  const std::string raw_nonce = std::to_string(realm->id) + ":" + random_hex + ":" + std::to_string(timestamp);

  unsigned char hmac_result[EVP_MAX_MD_SIZE];
  unsigned int hmac_len = 0;

  HMAC(EVP_sha256(), realm->nonce_secret.data(), static_cast<int>(realm->nonce_secret.size()), reinterpret_cast<const unsigned char*>(raw_nonce.data()),
       raw_nonce.size(), hmac_result, &hmac_len);

  const std::string hmac_hex = Util::to_hex(hmac_result, hmac_len);
  const std::string nonce = raw_nonce + ":" + hmac_hex;

  const auto expires_at = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now() + std::chrono::seconds(realm->nonce_expiry));

  auto self = shared_from_this();

  datastore->nonce_create(_strand, nonce, expires_at, [this, self, nonce, realm, handler](plugins::Status status) mutable {
    if (!status.ok) {
      _logger->error("Failed to generate nonce - " + status.error);
      if (handler) handler(plugins::Result<std::string>::failure(status.error));
      return;
    }

    _nonce_cache->add(nonce, realm->nonce_expiry * 1000);
    if (handler) handler(plugins::Result<std::string>::success(nonce));
  });
}

void Core::nonce_check(std::string nonce, plugins::Handler<bool> handler) { datastore->nonce_check(_strand, std::move(nonce), std::move(handler)); }

void Core::process_message(std::shared_ptr<SIPMessage> message) {
  _ensure_transaction_users();

  // A message without a valid start line, Via and CSeq names no transaction: 400.
  if (!message->header->is_valid() || !message->header->contains("Via") || !message->header->contains("CSeq")) {
    _logger->info("Malformed or incomplete message (needs a parseable start line, Via and CSeq) - 400");
    _send_status(message, 400, "Bad Request");
    return;
  }

  if (message->header->type == SIPHeader::Type::Response) {
    // RFC 3261 17.1.3: a response goes to the client transaction its branch names. One
    // that matches none is forwarded statelessly by the proxy (16.7 step 1).
    auto transaction = _matcher.match_response(message);

    if (!transaction) {
      _proxy->on_stray_response(message);
      return;
    }

    transaction->receive(message);
    return;
  }

  const auto& method = message->header->request_method;

  // RFC 3261 17.1.1.3: an ACK for a non-2xx is absorbed by its INVITE server transaction.
  // One that matches nothing is the end-to-end ACK for a 2xx and goes to the TU.
  if (method == "ACK") {
    auto transaction = _matcher.match_request(message);

    if (transaction) {
      transaction->receive(message);
      return;
    }

    _deliver_to_tu(message, nullptr);
    return;
  }

  // A retransmission is answered by its transaction and never reaches the TU.
  auto existing = _matcher.match_request(message);
  if (existing) {
    existing->receive(message);
    return;
  }

  _server_transaction_start(message);
}

void Core::_ensure_transaction_users() {
  if (_registrar && _proxy && _dialogs) return;

  auto base = _logger->base_logger();

  if (!_registrar) _registrar = std::make_shared<Registrar>(base, shared_from_this());
  if (!_proxy) _proxy = std::make_shared<Proxy>(base, shared_from_this());

  if (!_dialogs) {
    _dialogs = std::make_shared<Dialogs>(base, _timer_source);

    std::weak_ptr<Core> weak_self = weak_from_this();
    _dialogs->on_change([weak_self](const std::shared_ptr<types::Dialog>& dialog) {
      if (auto self = weak_self.lock()) self->_on_dialog_change(dialog);
    });
  }
}

std::shared_ptr<Dialogs> Core::dialogs() {
  _ensure_transaction_users();
  return _dialogs;
}

Core::Advertised Core::advertised_for(const Channel& channel) const {
  Advertised out;
  if (!channel._connection) return out;

  const auto local = channel._connection->local_endpoint();
  const auto remote = channel._connection->remote_endpoint();
  const auto transport = Util::to_lower(channel._connection->transport_name());

  // A peer node is always given the inter-node listener: the local end of a connection
  // this node opened is a port nothing listens on.
  if (!channel.peer_node().empty()) {
    if (const auto cluster = config->advertised_cluster()) {
      out.host = cluster->address;
      out.port = cluster->port;
      return out;
    }
  }

  if (!config->sip_public_address.empty() && !config->in_localnet(remote.address())) {
    const auto public_port = config->public_port_for(transport);
    out.host = config->sip_public_address;
    out.port = public_port != 0 ? public_port : local.port();
    return out;
  }

  out.host = local.address().to_string();
  out.port = local.port();

  // A wildcard listener: connect a UDP probe (nothing is sent) and take the local address
  // the kernel picks to reach that peer.
  if (local.address().is_unspecified()) {
    boost::system::error_code error;
    boost::asio::ip::udp::socket probe(detail::get_global_io_context());
    probe.open(remote.address().is_v4() ? boost::asio::ip::udp::v4() : boost::asio::ip::udp::v6(), error);
    if (!error) probe.connect(boost::asio::ip::udp::endpoint(remote.address(), remote.port() != 0 ? remote.port() : 9), error);
    if (!error) {
      const auto chosen = probe.local_endpoint(error);
      if (!error) out.host = chosen.address().to_string();
    }
  }

  return out;
}

void Core::push_register(std::shared_ptr<push::PushService> service) {
  if (service) _push_services[service->name()] = std::move(service);
}

std::shared_ptr<push::PushService> Core::push_service(const std::string& provider) const {
  const auto found = _push_services.find(provider);
  return found == _push_services.end() ? nullptr : found->second;
}

void Core::register_forward(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction) {
  if (_proxy) _proxy->forward_register(std::move(request), std::move(transaction));
}

void Core::binding_registered(std::uint64_t subscriber_id, const std::shared_ptr<SIPUri>& contact) {
  if (_proxy) _proxy->on_registered(subscriber_id, contact);
}

std::shared_ptr<PushRefresher> Core::push_refresher() {
  if (!_push_refresher) _push_refresher = std::make_shared<PushRefresher>(_logger->base_logger(), weak_from_this());
  return _push_refresher;
}

std::shared_ptr<Qualifier> Core::qualifier() {
  if (!_qualifier) _qualifier = std::make_shared<Qualifier>(_logger->base_logger(), weak_from_this());
  return _qualifier;
}

void Core::_deliver_to_tu(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction) {
  _ensure_transaction_users();

  // Retransmissions never get this far (RFC 3261 17.2.1), so the dialog tracker sees each
  // request once.
  request->in_known_dialog = !request->header->contains("To") ? false : _dialogs->find(request) != nullptr;
  _dialogs->observe_request(request);

  const auto& method = request->header->request_method;

  if (method == "REGISTER") {
    _registrar->on_request(request, transaction);
    return;
  }

  // RFC 3261 9.2: the TU gets the CANCEL's own transaction and the INVITE's it cancels.
  if (method == "CANCEL") {
    _proxy->on_cancel(request, transaction, _matcher.match_cancelled(request));
    return;
  }

  _proxy->on_request(request, transaction);
}

std::shared_ptr<transactions::TransactionBase> Core::_server_transaction_start(const std::shared_ptr<SIPMessage>& request) {
  const auto key = TransactionMatcher::key(request);

  if (key.empty()) {
    _logger->info("Request with no usable branch - 400");
    _send_status(request, 400, "Bad Request");
    return nullptr;
  }

  auto channel = request->channel.lock();

  // No retransmission timers on a reliable transport (RFC 3261 17.2.1, 17.2.2), or with no
  // channel to send on.
  const bool reliable = !channel || !channel->_connection || channel->_connection->is_reliable();

  std::weak_ptr<Channel> weak_channel = channel;
  std::weak_ptr<Core> weak_self = weak_from_this();

  auto send = [weak_channel](std::shared_ptr<SIPMessage> message) {
    if (auto target = weak_channel.lock()) target->send(message);
  };

  // The transaction is filed before it starts, so the lookup by key succeeds here.
  auto to_tu = [weak_self, key](std::shared_ptr<SIPMessage> message) {
    auto self = weak_self.lock();
    if (!self) return;

    self->_deliver_to_tu(message, self->_matcher.find(key));
  };

  const auto timers = Timers::from_config(*config);

  std::shared_ptr<transactions::TransactionBase> transaction;

  if (request->header->request_method == "INVITE") {
    transaction = std::make_shared<InviteServerTransaction>(_logger->base_logger(), key, reliable, timers, _timer_source, send, to_tu);
  } else {
    transaction = std::make_shared<NonInviteServerTransaction>(_logger->base_logger(), key, reliable, timers, _timer_source, send, to_tu);
  }

  transaction->on_terminated([weak_self](const std::string& id) {
    if (auto self = weak_self.lock()) self->transaction_remove(id);
  });

  transaction_add(key, transaction);

  if (request->header->request_method == "INVITE") {
    std::static_pointer_cast<InviteServerTransaction>(transaction)->start(request);
  } else {
    std::static_pointer_cast<NonInviteServerTransaction>(transaction)->start(request);
  }

  return transaction;
}

std::shared_ptr<transactions::TransactionBase> Core::client_transaction_start(std::shared_ptr<SIPMessage> request, std::shared_ptr<Channel> channel,
                                                                              transactions::TransactionBase::TuFn to_tu,
                                                                              transactions::TransactionBase::TimeoutFn on_timeout) {
  const auto key = TransactionMatcher::key(request);

  if (key.empty() || !channel) {
    _logger->error("Cannot start a client transaction without a branch and a flow");
    return nullptr;
  }

  const bool reliable = !channel->_connection || channel->_connection->is_reliable();

  std::weak_ptr<Channel> weak_channel = channel;
  std::weak_ptr<Core> weak_self = weak_from_this();

  auto send = [weak_channel](std::shared_ptr<SIPMessage> message) {
    if (auto target = weak_channel.lock()) target->send(message);
  };

  const auto timers = Timers::from_config(*config);

  std::shared_ptr<transactions::TransactionBase> transaction;

  if (request->header->request_method == "INVITE") {
    transaction = std::make_shared<InviteClientTransaction>(_logger->base_logger(), key, reliable, timers, _timer_source, send, std::move(to_tu));
  } else {
    transaction = std::make_shared<NonInviteClientTransaction>(_logger->base_logger(), key, reliable, timers, _timer_source, send, std::move(to_tu));
  }

  transaction->on_terminated([weak_self](const std::string& id) {
    if (auto self = weak_self.lock()) self->transaction_remove(id);
  });

  if (on_timeout) transaction->on_timeout(std::move(on_timeout));

  transaction_add(key, transaction);

  if (request->header->request_method == "INVITE") {
    std::static_pointer_cast<InviteClientTransaction>(transaction)->start(request);
  } else {
    std::static_pointer_cast<NonInviteClientTransaction>(transaction)->start(request);
  }

  return transaction;
}

void Core::_send_status(const std::shared_ptr<SIPMessage>& request, uint16_t code, const std::string& reason) {
  auto channel = request->channel.lock();

  if (!channel) {
    _logger->info("Channel has closed, cannot respond " + std::to_string(code));
    return;
  }

  auto response = request->generate_response();
  response->header->response_code = code;
  response->header->response_message = reason;

  channel->send(response);
}

void Core::transaction_add(const std::string& key, std::shared_ptr<transactions::TransactionBase> transaction) {
  _matcher.add(key, std::move(transaction));
  events->publish(events::topics::node_transaction(config->sip_node_id, key), "registered");
}

bool Core::transaction_remove(const std::string& key) {
  if (!_matcher.remove(key)) return false;

  events->publish(events::topics::node_transaction(config->sip_node_id, key), "unregistered");
  return true;
}

std::shared_ptr<transactions::TransactionBase> Core::transaction_get(const std::string& key) { return _matcher.find(key); }

std::size_t Core::transaction_count() const { return _matcher.size(); }

void Core::transaction_end_all() { _matcher.terminate_all(); }

bool Core::call_register(std::shared_ptr<Call> call) {
  _calls[call->id] = call;

  if (call->node.empty()) call->node = config->sip_node_id;

  // Only the node the caller reached writes the call record. Signalling does not wait on it.
  if (call->from_node.empty()) {
    datastore->call_create(_strand, call, [this, self = shared_from_this(), call](plugins::Status status) {
      if (!status.ok) _logger->error("Cannot store call " + call->id + " - " + status.error);
    });
  }

  events->publish(events::topics::call_register(call->id), call->id);

  return true;
}

// Keeps the Call (participants, media, record, events) in step with its dialog.
void Core::_on_dialog_change(const std::shared_ptr<types::Dialog>& dialog) {
  if (!dialog || dialog->call_id.empty()) return;

  auto call = call_get(dialog->call_id);

  if (!call) {
    // An attempt that failed before anyone answered: no call to record.
    if (dialog->state == types::Dialog::State::Terminated) return;

    call = std::make_shared<Call>();
    call->id = dialog->call_id;
    call->created_at = dialog->created_at != 0 ? dialog->created_at : std::time(nullptr);

    call->add_participant(dialog->caller, nullptr, true);
    call->add_participant(dialog->callee, nullptr, false);

    // add_participant's returned reference is invalidated by the next add, so set these after.
    for (auto& participant : call->participants) {
      participant.dialog = dialog;
      participant.node_id = config->sip_node_id;
    }

    // Forwarded here by a peer: the caller's leg and the record belong to that node.
    call->from_node = dialog->from_node;
    if (!call->from_node.empty()) call->participants.front().node_id = call->from_node;

    call_register(call);
  }

  const auto previous = call->state;

  switch (dialog->state) {
    case types::Dialog::State::Early:
      // A callee tag means the callee has responded.
      call->state = dialog->callee_tag.empty() ? Call::State::Trying : Call::State::Ringing;
      break;

    case types::Dialog::State::Confirmed:
      call->state = Call::State::Connected;
      if (call->answered_at == 0) call->answered_at = dialog->confirmed_at;
      break;

    case types::Dialog::State::Terminated:
      call->state = Call::State::Closed;
      call->ended_at = dialog->terminated_at;
      break;
  }

  if (call->state == previous) return;

  events->publish(events::topics::call_state(call->id), Call::state_to_string(call->state));

  if (call->from_node.empty()) {
    datastore->call_update(_strand, call, [this, self = shared_from_this(), call](plugins::Status status) {
      if (!status.ok) _logger->error("Cannot update call " + call->id + " - " + status.error);
    });
  }

  if (call->state != Call::State::Closed) return;

  // The dialog ending is the only signal that the call is over, so the media is released
  // here. The handler holds the call until the engine answers.
  if (media) {
    media->release(_strand, call, [this, self = shared_from_this(), call](plugins::Status status) {
      if (!status.ok) _logger->error("Cannot release the media for call " + call->id + " - " + status.error);
    });
  }

  call_unregister(call->id);
}

bool Core::call_unregister(std::string callId) {
  _calls.erase(callId);

  events->publish(events::topics::call_unregister(callId), callId);
  return true;
}

std::vector<std::shared_ptr<Call>> Core::call_list() const {
  std::vector<std::shared_ptr<Call>> calls;
  calls.reserve(_calls.size());
  for (const auto& [id, call] : _calls) {
    if (call) calls.push_back(call);
  }
  return calls;
}

std::map<std::string, std::size_t> Core::channel_counts() const {
  std::set<const Channel*> seen;
  std::map<std::string, std::size_t> counts;

  for (const auto& [name, channel] : _channels) {
    if (!channel || !channel->_connection || !seen.insert(channel.get()).second) continue;
    counts[Util::to_lower(channel->_connection->transport_name())]++;
  }

  return counts;
}

std::shared_ptr<Call> Core::call_get(std::string callId) {
  auto search = _calls.find(callId);
  if (search == _calls.end()) return nullptr;
  return search->second;
}

void Core::media_register(std::shared_ptr<media::MediaEngine> engine) {
  media = std::move(engine);

  // The media half of the call sweep needs an engine.
  _call_sweep_schedule();
}

// The status is published on an interval and retained, so a late monitor still learns it;
// the bus's last-will message covers a node that dies.
void Core::node_status_start() {
  _started_at = std::time(nullptr);

  // Hear every node's retained status, to know the cluster.
  std::weak_ptr<NodeDirectory> weak_nodes = _nodes;
  events->subscribe(
      _strand, "nodes/+/status",
      [weak_nodes](std::string topic, std::string message) {
        if (auto nodes = weak_nodes.lock()) nodes->observe(topic, message);
      },
      [this, self = shared_from_this()](plugins::Result<std::shared_ptr<events::Subscription>> subscribed) {
        if (!subscribed.ok) _logger->warn("Cannot listen for the other nodes - " + subscribed.error);
      });

  _node_status_publish();
}

void Core::node_status_stop() {
  if (_node_status_timer) {
    _node_status_timer->cancel();
    _node_status_timer.reset();
  }

  events->publish_state(events::topics::node_status(config->sip_node_id), node_status_json("stopped"));
}

std::string Core::node_status_json(const std::string& status, const std::string& node_id, const std::string& version, const std::string& datastore,
                                   std::int64_t uptime, std::uint32_t status_interval, const std::vector<Config::AdvertisedTransport>& transports,
                                   const std::optional<Config::AdvertisedTransport>& cluster) {
  boost::json::object report;

  report["status"] = status;
  report["node"] = node_id;
  report["version"] = version;
  report["datastore"] = datastore;
  report["at"] = Util::get_zulu_time();
  report["uptime"] = uptime;

  // When the next report is due, in seconds, so a monitor can judge staleness. Zero means
  // the node does not repeat it.
  report["status_interval"] = status_interval;

  // Where the node listens, which makes the status a directory entry.
  boost::json::array listening;
  for (const auto& transport : transports) {
    boost::json::object entry;
    entry["transport"] = transport.transport;
    entry["address"] = transport.address;
    entry["port"] = transport.port;
    entry["uri"] = transport.uri();
    listening.push_back(std::move(entry));
  }
  report["transports"] = std::move(listening);

  // The inter-node listener. Absent outside a cluster, and in a will.
  if (cluster) {
    boost::json::object peer;
    peer["address"] = cluster->address;
    peer["port"] = cluster->port;
    report["cluster"] = std::move(peer);
  }

  return boost::json::serialize(report);
}

std::string Core::node_status_json(const std::string& status) const {
  const auto uptime = _started_at == 0 ? 0 : static_cast<std::int64_t>(std::time(nullptr) - _started_at);

  auto report = boost::json::parse(node_status_json(status, config->sip_node_id, _version, datastore ? datastore->describe() : "none", uptime,
                                                    config->events_status_interval, config->advertised_transports(), config->advertised_cluster()))
                    .as_object();

  // The media engine and what it can do. Null, not absent, for a node with no engine.
  if (media) {
    boost::json::object engine;
    engine["engine"] = media->describe();

    const auto capabilities = media->capabilities();
    boost::json::array can;
    if (capabilities.bridge) can.push_back("bridge");
    if (capabilities.conference) can.push_back("conference");
    if (capabilities.record) can.push_back("record");
    if (capabilities.transcode) can.push_back("transcode");
    engine["capabilities"] = std::move(can);

    boost::json::array produces;
    for (const auto profile : {media::Profile::PlainRtp, media::Profile::WebRtc, media::Profile::SrtpSdes}) {
      if (media->produces(profile)) produces.push_back(boost::json::string(media::setting_name(profile)));
    }
    engine["produces"] = std::move(produces);

    report["media"] = std::move(engine);
  } else {
    report["media"] = nullptr;
  }

  return boost::json::serialize(report);
}

void Core::_node_status_publish() {
  // A node without its datastore is running but not serving: degraded.
  const auto status = (datastore && datastore->is_connected()) ? "ok" : "degraded";

  events->publish_state(events::topics::node_status(config->sip_node_id), node_status_json(status));

  _node_status_schedule();
}

void Core::_node_status_schedule() {
  if (_node_status_timer) {
    _node_status_timer->cancel();
    _node_status_timer.reset();
  }

  if (config->events_status_interval == 0) return;

  std::weak_ptr<Core> weak_self = weak_from_this();

  _node_status_timer = _timer_source->schedule(std::chrono::seconds(config->events_status_interval), [weak_self]() {
    if (auto self = weak_self.lock()) self->_node_status_publish();
  });
}

// A UDP flow is created by the first datagram from an address and nothing ends it, so idle
// ones are closed here; otherwise spoofed source addresses could grow the registry without
// bound. Reliable flows end with their socket and are never swept.
void Core::flow_sweep_start() { _flow_sweep_schedule(); }

void Core::_flow_sweep_schedule() {
  if (_flow_sweep_timer) {
    _flow_sweep_timer->cancel();
    _flow_sweep_timer.reset();
  }

  const auto timeout = config->sip_flow_idle_timeout;
  if (timeout == 0) return;

  // Every quarter of the timeout, and at most every fifteen seconds.
  const auto interval = std::max<std::uint32_t>(timeout / 4, 15);

  std::weak_ptr<Core> weak_self = weak_from_this();

  _flow_sweep_timer = _timer_source->schedule(std::chrono::seconds(interval), [weak_self]() {
    if (auto self = weak_self.lock()) self->_flow_sweep();
  });
}

void Core::_flow_sweep() {
  _flow_sweep_timer.reset();

  const auto timeout = config->sip_flow_idle_timeout;

  if (timeout > 0) {
    // The injectable clock, which is also what Channel::touch stamps.
    const auto now = _timer_source->now();

    // close() erases from _channels, so collect first.
    std::vector<std::shared_ptr<Channel>> idle;

    for (const auto& [endpoint, channel] : _channels) {
      if (!channel || !channel->_connection) continue;

      if (channel->_connection->is_reliable()) continue;

      const auto quiet_for = std::chrono::duration_cast<std::chrono::seconds>(now - channel->last_activity()).count();
      if (quiet_for < static_cast<std::int64_t>(timeout)) continue;

      // A channel can be filed under several names.
      if (std::find(idle.begin(), idle.end(), channel) == idle.end()) idle.push_back(channel);
    }

    for (const auto& channel : idle) {
      _logger->debug("Forgetting idle flow " + channel->flow_id());
      channel->close();
    }
  }

  _flow_sweep_schedule();
}

void Core::_call_sweep_schedule() {
  if (_call_sweep_timer) {
    _call_sweep_timer->cancel();
    _call_sweep_timer.reset();
  }

  const auto media_timeout = media ? config->sip_media_timeout : 0;
  const auto max_duration = config->sip_max_call_duration;

  if (media_timeout == 0 && max_duration == 0) return;

  // Every quarter of the shorter bound, and at most every fifteen seconds: each pass
  // queries the engine once per live call.
  std::uint32_t shortest = media_timeout;
  if (max_duration > 0 && (shortest == 0 || max_duration < shortest)) shortest = max_duration;

  const auto interval = std::max<std::uint32_t>(shortest / 4, 15);

  std::weak_ptr<Core> weak_self = weak_from_this();

  _call_sweep_timer = _timer_source->schedule(std::chrono::seconds(interval), [weak_self]() {
    if (auto self = weak_self.lock()) self->_call_sweep();
  });
}

void Core::_call_sweep() {
  _call_sweep_timer.reset();

  const auto media_timeout = config->sip_media_timeout;
  const auto max_duration = config->sip_max_call_duration;
  const auto now = _timer_source->now();

  const bool ask_media = media_timeout > 0 && media && media->is_connected();

  // Confirmed dialogs only: a call being set up has no media yet and is bounded by timers
  // B and C.
  std::unordered_set<std::string> seen;

  for (const auto& dialog : dialogs()->all()) {
    if (!dialog || dialog->state != types::Dialog::State::Confirmed) continue;
    if (!seen.insert(dialog->call_id).second) continue;

    // The duration cap applies to every call, anchored or not.
    if (max_duration > 0 && dialog->confirmed_monotonic.time_since_epoch().count() != 0) {
      const auto up_for = std::chrono::duration_cast<std::chrono::seconds>(now - dialog->confirmed_monotonic).count();

      if (up_for >= static_cast<std::int64_t>(max_duration)) {
        _end_held_call(dialog->call_id, "has been up for " + std::to_string(up_for) + "s, which is the configured maximum");
        continue;
      }
    }

    if (!ask_media) continue;

    auto call = call_get(dialog->call_id);
    if (!call) continue;

    std::weak_ptr<Core> weak_self = weak_from_this();
    const auto call_id = dialog->call_id;

    media->query(strand(), call, [weak_self, call_id, media_timeout](plugins::Result<std::string> held) {
      auto self = weak_self.lock();
      if (!self || !held.ok) return;

      const auto idle = idle_seconds_of(held.value);

      // No reading leaves the call alone.
      if (!idle.has_value() || *idle < media_timeout) return;

      self->_end_held_call(call_id, "has carried no media for " + std::to_string(*idle) + "s");
    });
  }

  _call_sweep_schedule();
}

void Core::_end_held_call(const std::string& call_id, const std::string& reason) {
  _logger->info("Call " + call_id + " " + reason + " - letting it go");

  // Terminating the dialogs is enough: _on_dialog_change writes the record and releases the
  // media. No BYE is sent (RFC 4028 section 8.3).
  for (const auto& dialog : dialogs()->all()) {
    if (dialog && dialog->call_id == call_id) dialogs()->terminate(dialog);
  }
}

void Core::admin_register(std::shared_ptr<api::AdminAPI> adminAPI) { _adminAPI = adminAPI; }

void Core::admin_start() {
  if (_adminAPI) _adminAPI->start();
}

void Core::admin_stop() {
  if (_adminAPI) _adminAPI->stop();
}

}  // namespace athenasip
