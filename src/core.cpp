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

// The engine's query() answers with a JSON document, and idle_seconds is how long every
// relay it holds for the call has been silent. Absent, null or unparseable all mean the
// same thing here: this engine is not saying, so nothing is decided from it.
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

// Realms
void Core::realm_get_by_name(std::string realm_name, plugins::Handler<std::shared_ptr<Realm>> handler) {
  datastore->realm_get_by_name(_strand, std::move(realm_name), std::move(handler));
}

// Subscribers
void Core::subscriber_get(std::shared_ptr<SIPIdentity> identity, plugins::Handler<std::shared_ptr<Subscriber>> handler) {
  datastore->subscriber_get(_strand, std::move(identity), std::move(handler));
}

void Core::location_list(std::uint64_t subscriber_id, plugins::Handler<std::vector<types::Location>> handler) {
  datastore->location_list(_strand, subscriber_id, std::move(handler));
}

void Core::subscriber_register(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel,
                               std::uint32_t expires_seconds, std::string path, plugins::StatusHandler handler, std::string instance, std::uint32_t reg_id) {
  // RFC 3261 10.3 step 7: the binding is written on every successful REGISTER. This
  // used to be skipped whenever the subscriber record already existed, which is always,
  // so no contact was ever stored and the registrar had nothing to route to.
  //
  // What the node knows and the Contact does not: the flow the REGISTER arrived over and
  // that this node is the one holding it. A browser or a NAT'd client has a Contact that
  // resolves to nothing reachable, so the flow is the only way back to it (RFC 5626), and
  // a second node has to know whose flow it is before it can ask for it.
  types::Location binding;
  binding.contact = contact;
  binding.path = std::move(path);
  binding.node_id = config->sip_node_id;
  binding.instance = std::move(instance);
  binding.reg_id = reg_id;
  if (channel) binding.flow_id = channel->flow_id();

  // The channel index and the event both wait for the write: a binding nobody stored is
  // not one to announce.
  datastore->subscriber_register(
      _strand, subscriber, std::move(binding), expires_seconds, [this, subscriber, contact, channel, handler](plugins::Status status) mutable {
        if (!status.ok) {
          _logger->error("Cannot register subscriber " + subscriber->identity->to_string() + " - " + status.error);
          if (handler) handler(status);
          return;
        }

        events->publish(
            events::topics::subscriber_status(subscriber->identity->uri->to_string()),
            "{\"contact\":\"" + contact->to_string() + "\",\"node\":\"" + config->sip_node_id + "\",\"registered\":\"" + Util::get_zulu_time() + "\"}");

        if (handler) handler(status);
      });
}

void Core::subscriber_unregister(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel,
                                 plugins::StatusHandler handler) {
  (void)channel;

  auto self = shared_from_this();

  datastore->subscriber_unregister(_strand, subscriber, contact, [this, self, subscriber, handler](plugins::Status status) mutable {
    if (!status.ok) _logger->error("Cannot unregister subscriber " + subscriber->identity->to_string() + " - " + status.error);
    if (handler) handler(status);
  });
}

// Channels

std::string Core::channel_key(const std::string& transport, const std::string& host, std::uint16_t port) {
  return channel_key(transport, host + ":" + std::to_string(port));
}

std::string Core::channel_key(const std::string& transport, const std::string& endpoint) { return Util::to_lower(transport) + "://" + endpoint; }

bool Core::channel_register(std::string endpoint, std::shared_ptr<Channel> channel) {
  _channels[endpoint] = channel;
  _channels_by_token[channel->flow_token()] = channel;

  // Where the far end reached us is where a Record-Route this node writes will point,
  // so it is what a Route coming back has to be recognised against (RFC 3261 16.4).
  if (channel->_connection) {
    local_address_add(channel->_connection->local_endpoint_name());

    // And the address it advertises on that flow, which is the one it actually wrote.
    // Without this the Record-Route comes back as a Route naming the public address,
    // the node does not know itself in its own route set, and it forwards the request
    // to itself - a loop, caught by 16.3.4 as a 482 instead of routing the BYE.
    const auto local = channel->_connection->local_endpoint();
    const auto advertised = advertised_for(*channel);
    local_address_add(advertised.host + ":" + std::to_string(advertised.port));

    // And the public name, which a Route from outside will carry whatever this flow is.
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

  // Every name, not only the one the caller knew. A dialled channel is filed under the
  // address it resolved to and under the name it was asked for, and leaving the second
  // behind would be a route to a closed socket.
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

// RFC 3261 16.6 step 7 and 18.1. Everything here runs on the global io_context, which is
// where an outbound socket belongs - it has no server of its own - and the answer is
// posted back to the strand, which is where the registry lives.
void Core::channel_connect(std::string transport, std::string host, std::uint16_t port, plugins::Handler<std::shared_ptr<Channel>> handler) {
  using ChannelResult = plugins::Result<std::shared_ptr<Channel>>;

  transport = Util::to_lower(transport);

  const auto key = channel_key(transport, host, port);

  if (auto existing = channel_find(transport, host, port)) return handler(ChannelResult::success(existing));

  if (transport == "udp") return _connect_datagram(host, port, std::move(handler));

  // TLS outbound is to another node of the cluster, with the cluster's certificates, and
  // to nothing else: what a node trusts beyond its own cluster is not a guess to make here.
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

  // One answer only. The timer and the connect race each other, and whichever loses must
  // not call the handler a second time - a transaction told twice that its hop is
  // unreachable would try the next target twice.
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

  // Not RFC 3263: no NAPTR and no SRV, only the A and AAAA records for the host the URI
  // named. The service records are a step of their own, and what a cluster and a trunk
  // both need.
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

          // start() dispatches onto the strand and files the channel under the address it
          // reached, which is not the name it was asked for when that name was a hostname.
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

// The client half of the cluster's mutual TLS: this node's certificate shown, the peer's
// checked against the cluster CA and against the address or name that was dialled, so a
// member of the cluster cannot answer for another one.
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

// UDP has no connection to open. A datagram to a host this node has never heard from has to
// leave by a listener's own socket, so that the source port is the one the far end answers
// to and the one a NAT in front of it already has a mapping for (RFC 3261 18.1.1, RFC 3581).
// The listener makes the connection; the channel over it is made here, as for TCP.
void Core::_connect_datagram(std::string host, std::uint16_t port, plugins::Handler<std::shared_ptr<Channel>> handler) {
  using ChannelResult = plugins::Result<std::shared_ptr<Channel>>;

  const auto key = channel_key("udp", host, port);

  // A literal address, which is every flow this node is reopening and most Contacts, needs
  // no resolver and no trip off the strand.
  boost::system::error_code literal;
  const auto address = boost::asio::ip::make_address(host, literal);
  if (!literal) return _open_datagram({boost::asio::ip::udp::endpoint(address, port)}, key, std::move(handler));

  auto& io_context = detail::get_global_io_context();
  auto resolver = std::make_shared<boost::asio::ip::udp::resolver>(io_context);
  auto deadline = std::make_shared<boost::asio::steady_timer>(io_context);

  // Bounded as the TCP dial is: a name can hang for as long as the system resolver likes,
  // and nothing else is timing this request yet. Whichever answers first is the answer, and
  // it is given on the strand.
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

// On the strand, which is where the server list is read.
void Core::_open_datagram(std::vector<boost::asio::ip::udp::endpoint> candidates, std::string key, plugins::Handler<std::shared_ptr<Channel>> handler) {
  using ChannelResult = plugins::Result<std::shared_ptr<Channel>>;

  auto self = shared_from_this();

  for (const auto& remote : candidates) {
    for (const auto& server : _servers) {
      const bool asked = server->open_datagram_flow(remote, [self, remote, key, handler](std::shared_ptr<servers::Connection> connection) {
        if (!connection) {
          // A datagram from the peer made its flow first. That flow's channel is the one, if
          // it has registered by now; if not, this attempt fails and the next request finds it.
          if (auto existing = self->channel_find("udp", remote.address().to_string(), remote.port())) return handler(ChannelResult::success(existing));
          return handler(ChannelResult::failure("a flow to " + key + " was being made by a datagram from it"));
        }

        auto channel = std::make_shared<Channel>(self->_logger->base_logger(), self, connection);

        // On the strand already, so this registers before the answer goes back.
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
  // close() unregisters, which erases from _channels. Take a copy and empty the map
  // first so nothing mutates it while we are walking it.
  std::vector<std::shared_ptr<Channel>> channels;
  channels.reserve(_channels.size());
  for (const auto& [endpoint, channel] : _channels) channels.push_back(channel);
  _channels.clear();
  _channels_by_token.clear();

  for (const auto& channel : channels) channel->close();
}

// Nonce

void Core::nonce_create(std::shared_ptr<Realm> realm, plugins::Handler<std::string> handler) {
  std::array<unsigned char, 16> random_bytes;

  if (RAND_bytes(random_bytes.data(), random_bytes.size()) != 1) {
    throw std::runtime_error("Failed to generate secure random bytes");
  }

  const uint64_t timestamp = static_cast<uint64_t>(std::time(nullptr));

  const std::string random_hex = Util::to_hex(random_bytes.data(), random_bytes.size());

  // Public - This is visible to the client.
  const std::string raw_nonce = std::to_string(realm->id) + ":" + random_hex + ":" + std::to_string(timestamp);

  // Sign the public nonce material using the realm secret.
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

    // Cache the actual nonce for this node
    _nonce_cache->add(nonce, realm->nonce_expiry * 1000);
    if (handler) handler(plugins::Result<std::string>::success(nonce));
  });
}

void Core::nonce_check(std::string nonce, plugins::Handler<bool> handler) { datastore->nonce_check(_strand, std::move(nonce), std::move(handler)); }

// Messages

void Core::process_message(std::shared_ptr<SIPMessage> message) {
  _ensure_transaction_users();

  // RFC 3261 8.2.1: a request this node cannot parse gets a 400 rather than silence.
  // Via and CSeq are what name the transaction, so without them there is nothing to
  // route to either.
  if (!message->header->is_valid() || !message->header->contains("Via") || !message->header->contains("CSeq")) {
    _logger->info("Malformed or incomplete message (needs a parseable start line, Via and CSeq) - 400");
    _send_status(message, 400, "Bad Request");
    return;
  }

  if (message->header->type == SIPHeader::Type::Response) {
    // RFC 3261 17.1.3: a response belongs to the client transaction whose branch it
    // carries. One that belongs to none has no context here, so it goes back down its
    // Via chain statelessly (16.7 step 1, 18.1.2), which is the proxy's job.
    auto transaction = _matcher.match_response(message);

    if (!transaction) {
      _proxy->on_stray_response(message);
      return;
    }

    transaction->receive(message);
    return;
  }

  const auto& method = message->header->request_method;

  // RFC 3261 17.1.1.3: an ACK for a non-2xx belongs to the INVITE server transaction
  // that sent the response, which absorbs it. An ACK that matches nothing is the ACK for
  // a 2xx, which is end to end and goes straight to the TU.
  if (method == "ACK") {
    auto transaction = _matcher.match_request(message);

    if (transaction) {
      transaction->receive(message);
      return;
    }

    _deliver_to_tu(message, nullptr);
    return;
  }

  // A request that matches an existing transaction is a retransmission. The transaction
  // answers it from what it last sent; the TU never sees it twice.
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

  if (!config->sip_public_address.empty() && !config->in_localnet(remote.address())) {
    const auto public_port = config->public_port_for(transport);
    out.host = config->sip_public_address;
    out.port = public_port != 0 ? public_port : local.port();
    return out;
  }

  out.host = local.address().to_string();
  out.port = local.port();

  // The kernel's answer to "which of my addresses would reach that peer": a connected UDP
  // socket sends nothing, but has a local address once connected.
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

std::shared_ptr<Qualifier> Core::qualifier() {
  if (!_qualifier) _qualifier = std::make_shared<Qualifier>(_logger->base_logger(), weak_from_this());
  return _qualifier;
}

void Core::_deliver_to_tu(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction) {
  _ensure_transaction_users();

  // Here rather than in process_message, because this is where a request has been
  // de-duplicated: a retransmission is absorbed by its transaction and never arrives
  // (17.2.1), so what the tracker sees is each request once.
  request->in_known_dialog = !request->header->contains("To") ? false : _dialogs->find(request) != nullptr;
  _dialogs->observe_request(request);

  const auto& method = request->header->request_method;

  if (method == "REGISTER") {
    _registrar->on_request(request, transaction);
    return;
  }

  // RFC 3261 9.2: a CANCEL is its own transaction and separately names the INVITE
  // transaction it cancels, so the TU is handed both.
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

  // A stream transport neither loses nor duplicates, so the retransmission timers are
  // pointless on one (RFC 3261 17.2.1, 17.2.2). Nothing to send on means nothing to
  // retransmit either.
  const bool reliable = !channel || !channel->_connection || channel->_connection->is_reliable();

  std::weak_ptr<Channel> weak_channel = channel;
  std::weak_ptr<Core> weak_self = weak_from_this();

  auto send = [weak_channel](std::shared_ptr<SIPMessage> message) {
    if (auto target = weak_channel.lock()) target->send(message);
  };

  // The transaction is filed before it is started, so by the time the TU is called it can
  // be found by the key it will answer on.
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

// Transactions

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

// Calls
bool Core::call_register(std::shared_ptr<Call> call) {
  _calls[call->id] = call;

  // The call record is for the admin API and the cluster, not for this call's
  // signalling, so nothing waits on it.
  datastore->call_create(_strand, call, [this, self = shared_from_this(), call](plugins::Status status) {
    if (!status.ok) _logger->error("Cannot store call " + call->id + " - " + status.error);
  });

  events->publish(events::topics::call_register(call->id), call->id);

  return true;
}

// A dialog is the signalling relationship between the two ends; a Call is the
// application object that hangs off it - participants, media, focus - and what the admin
// API lists and the event bus announces. One follows the other, which is the whole
// reason this node tracks dialogs it does not own.
void Core::_on_dialog_change(const std::shared_ptr<types::Dialog>& dialog) {
  if (!dialog || dialog->call_id.empty()) return;

  auto call = call_get(dialog->call_id);

  if (!call) {
    // A dialog that is over before this node had a call for it is an attempt that failed
    // before anyone answered. There is no call to record.
    if (dialog->state == types::Dialog::State::Terminated) return;

    call = std::make_shared<Call>();
    call->id = dialog->call_id;
    call->created_at = dialog->created_at != 0 ? dialog->created_at : std::time(nullptr);

    call->add_participant(dialog->caller, nullptr, true);
    call->add_participant(dialog->callee, nullptr, false);

    // Not inside add_participant: it returns a reference into the vector, and the second
    // call reallocates it.
    for (auto& participant : call->participants) {
      participant.dialog = dialog;
      participant.node_id = config->sip_node_id;
    }

    call_register(call);
  }

  const auto previous = call->state;

  switch (dialog->state) {
    case types::Dialog::State::Early:
      // A callee tag means the callee has spoken, which is the difference between a call
      // that is on its way and one that is ringing.
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

  // The record is for the admin API and the cluster, not for this call's signalling, so
  // nothing waits on it.
  datastore->call_update(_strand, call, [this, self = shared_from_this(), call](plugins::Status status) {
    if (!status.ok) _logger->error("Cannot update call " + call->id + " - " + status.error);
  });

  if (call->state != Call::State::Closed) return;

  // The other end of the anchoring the proxy does on the signalling path. A dialog
  // ending is the only thing that says a call is over, which is the whole reason this
  // node tracks dialogs it does not own; the ports go back here or they never do.
  //
  // The call is held by the handler, so unregistering it below does not take it away
  // from an engine that has not answered yet.
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

// Media

void Core::media_register(std::shared_ptr<media::MediaEngine> engine) {
  media = std::move(engine);

  // Nothing to ask an engine until there is one, and the media half of the sweep is the
  // half that needs it.
  _call_sweep_schedule();
}

// A node saying it is alive, on an interval, retained, with the broker primed to say
// otherwise if it vanishes. Those three together are what makes this answerable by a
// monitor: the interval proves it is still running, retention means a monitor that
// arrives late still learns the answer, and the will covers the case where the node
// never gets to speak again.
void Core::node_status_start() {
  _started_at = std::time(nullptr);

  // Every node's status, so that this one knows the cluster. Retained, so a node that
  // starts late hears the others at once rather than an interval later.
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
                                   std::int64_t uptime, std::uint32_t status_interval, const std::vector<Config::AdvertisedTransport>& transports) {
  boost::json::object report;

  report["status"] = status;
  report["node"] = node_id;
  report["version"] = version;
  report["datastore"] = datastore;
  report["at"] = Util::get_zulu_time();
  report["uptime"] = uptime;

  // The promise of when it will be said again, so a monitor derives its staleness from the
  // node rather than from a constant that agrees with it by coincidence. Zero is a node that
  // does not repeat itself.
  report["status_interval"] = status_interval;

  // Where it listens, which is what makes the status a directory entry and not only a
  // heartbeat.
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

  return boost::json::serialize(report);
}

std::string Core::node_status_json(const std::string& status) const {
  const auto uptime = _started_at == 0 ? 0 : static_cast<std::int64_t>(std::time(nullptr) - _started_at);

  return node_status_json(status, config->sip_node_id, _version, datastore ? datastore->describe() : "none", uptime, config->events_status_interval,
                          config->advertised_transports());
}

void Core::_node_status_publish() {
  // Degraded rather than ok where the datastore is not there: a node that cannot read a
  // registration is running but is not serving, and a health report that called that ok
  // would be the most misleading thing this node says.
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

// A UDP flow is made by the first datagram from an address and has nothing to end it: no
// socket, no close, no error on the read. Without this the channel registry and the
// server's own map grow for the life of the process, no `closed` is ever published for a
// UDP flow, and anything counting channels counts wrongly and forever - and the map is
// keyed by a remote address a datagram can claim to be from, so on a public listener it is
// a way to grow a node's memory from off the network.
//
// Only unreliable flows are swept. A TCP, TLS or WebSocket flow ends when its socket does,
// and sweeping one that is merely quiet would close a registration's path home.
void Core::flow_sweep_start() { _flow_sweep_schedule(); }

void Core::_flow_sweep_schedule() {
  if (_flow_sweep_timer) {
    _flow_sweep_timer->cancel();
    _flow_sweep_timer.reset();
  }

  const auto timeout = config->sip_flow_idle_timeout;
  if (timeout == 0) return;

  // A quarter of the timeout, never more often than every fifteen seconds: a pass is a walk
  // over every live channel, and being a quarter late to forget one costs nothing.
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
    // The injectable clock, as the call sweep uses: an idle timeout measured against the
    // real one is five minutes of waiting per test case. In production it is
    // steady_clock::now(), which is exactly what Channel::touch stamps.
    const auto now = _timer_source->now();

    // Collected before anything is closed: closing a channel unregisters it, which erases
    // from the map being walked.
    std::vector<std::shared_ptr<Channel>> idle;

    for (const auto& [endpoint, channel] : _channels) {
      if (!channel || !channel->_connection) continue;

      // A reliable transport has a socket to tell us, and its silence means nothing.
      if (channel->_connection->is_reliable()) continue;

      const auto quiet_for = std::chrono::duration_cast<std::chrono::seconds>(now - channel->last_activity()).count();
      if (quiet_for < static_cast<std::int64_t>(timeout)) continue;

      // One channel can be filed under several names. Closing it once is enough.
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

  // A quarter of whichever bound is shorter, so a call is noticed within a quarter of the
  // limit that catches it, and never more often than every fifteen seconds: each pass is
  // a walk over every live call and a round trip to the engine for each one of them.
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

  // Confirmed dialogs only. A call still being set up has relay ports and no media by
  // definition - nothing flows until somebody answers - and what bounds that is timer C
  // and timer B, not this.
  std::unordered_set<std::string> seen;

  for (const auto& dialog : dialogs()->all()) {
    if (!dialog || dialog->state != types::Dialog::State::Confirmed) continue;
    if (!seen.insert(dialog->call_id).second) continue;

    // The cap first, because it needs nothing but the clock and it applies to calls the
    // media question cannot reach.
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

      // No reading is not the same as a long one. An engine holding nothing for this
      // call, or one whose query says nothing about idleness, leaves the call alone.
      if (!idle.has_value() || *idle < media_timeout) return;

      self->_end_held_call(call_id, "has carried no media for " + std::to_string(*idle) + "s");
    });
  }

  _call_sweep_schedule();
}

void Core::_end_held_call(const std::string& call_id, const std::string& reason) {
  _logger->info("Call " + call_id + " " + reason + " - letting it go");

  // Terminating the dialogs is the whole of it: the change callback is what writes the
  // call record and releases the engine's ports, exactly as it does when a session timer
  // lapses. No BYE goes anywhere (RFC 4028 section 8.3).
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
