//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "core.h"

#include "channel.h"
#include "events/topics.h"
#include "expiry_set.h"
#include "proxy.h"
#include "registrar.h"
#include "rtp/rtp_relay.h"
#include "transactions/invite_client_transaction.h"
#include "transactions/invite_server_transaction.h"
#include "transactions/non_invite_client_transaction.h"
#include "transactions/non_invite_server_transaction.h"
#include "types/sip_uri.h"

using namespace athenasip::servers;
using namespace athenasip::datastores;
using namespace athenasip::events;
using namespace athenasip::rtp;
using namespace athenasip::types;
using namespace athenasip::transactions;

namespace athenasip {

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
                               std::uint32_t expires_seconds, std::string path, plugins::StatusHandler handler) {
  // RFC 3261 10.3 step 7: the binding is written on every successful REGISTER. This
  // used to be skipped whenever the subscriber record already existed, which is always,
  // so no contact was ever stored and the registrar had nothing to route to.
  //
  // The channel index and the event both wait for the write: a binding nobody stored is
  // not one to announce.
  datastore->subscriber_register(
      _strand, subscriber, contact, expires_seconds, std::move(path), [this, subscriber, contact, channel, handler](plugins::Status status) mutable {
        if (!status.ok) {
          _logger->error("Cannot register subscriber identity " + subscriber->identity->to_string() + " - " + status.error);
          if (handler) handler(status);
          return;
        }

        if (channel) _channels_by_subscriber[subscriber->id] = channel;

        events->publish(
            events::topics::subscriber_status(subscriber->identity->uri->to_string()),
            "{\"contact\":\"" + contact->to_string() + "\",\"node\":\"" + config->sip_node_id + "\",\"registered\":\"" + Util::get_zulu_time() + "\"}");

        if (handler) handler(status);
      });
}

void Core::subscriber_unregister(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel,
                                 plugins::StatusHandler handler) {
  (void)channel;

  _channels_by_subscriber.erase(subscriber->id);

  auto self = shared_from_this();

  datastore->subscriber_unregister(_strand, subscriber, contact, [this, self, subscriber, handler](plugins::Status status) mutable {
    if (!status.ok) _logger->error("Cannot unregister subscriber identity " + subscriber->identity->to_string() + " - " + status.error);
    if (handler) handler(status);
  });
}

std::shared_ptr<Channel> Core::subscriber_get_channel(std::shared_ptr<Subscriber> subscriber) {
  // operator[] would insert an empty entry for an unknown subscriber.
  auto search = _channels_by_subscriber.find(subscriber->id);
  if (search == _channels_by_subscriber.end()) return nullptr;
  return search->second;
}

// Channels

bool Core::channel_register(std::string endpoint, std::shared_ptr<Channel> channel) {
  _channels[endpoint] = channel;

  // Where the far end reached us is where a Record-Route this node writes will point,
  // so it is what a Route coming back has to be recognised against (RFC 3261 16.4).
  if (channel->_connection) local_address_add(channel->_connection->local_endpoint_name());

  events->publish(events::topics::node_channel(config->sip_node_id, channel->_connection->transport_name(), channel->_connection->remote_endpoint_name()),
                  "{\"status\":\"registered\",\"at\":\"" + Util::get_zulu_time() + "\"}");

  _logger->debug("Registered Channel " + endpoint);
  return true;
}

bool Core::channel_unregister(std::string endpoint, std::shared_ptr<Channel> channel) {
  events->publish(events::topics::node_channel(config->sip_node_id, channel->_connection->transport_name(), channel->_connection->remote_endpoint_name()),
                  "{\"status\":\"closed\",\"at\":\"" + Util::get_zulu_time() + "\"}");

  _channels.erase(endpoint);
  _logger->debug("Unregistered Channel " + endpoint);
  return true;
}

std::shared_ptr<Channel> Core::channel_find(const std::string& transport, const std::string& host, std::uint16_t port) {
  auto search = _channels.find(Util::to_lower(transport) + "://" + host + ":" + std::to_string(port));
  if (search == _channels.end()) return nullptr;
  return search->second;
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
  if (_registrar && _proxy) return;

  auto base = _logger->base_logger();

  if (!_registrar) _registrar = std::make_shared<Registrar>(base, shared_from_this());
  if (!_proxy) _proxy = std::make_shared<Proxy>(base, shared_from_this());
}

void Core::_deliver_to_tu(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction) {
  _ensure_transaction_users();

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

bool Core::call_unregister(std::string callId) {
  _calls.erase(callId);

  events->publish(events::topics::call_unregister(callId), callId);
  return true;
}

std::shared_ptr<Call> Core::call_get(std::string callId) {
  auto search = _calls.find(callId);
  if (search == _calls.end()) return nullptr;
  return search->second;
}

// Media

void Core::media_register(std::shared_ptr<media::MediaEngine> engine) { media = std::move(engine); }

// RTP Relays

void Core::rtprelay_register(std::shared_ptr<rtp::RTPRelay> relay) { _rtprelay = relay; }

void Core::rtprelay_start() {
  if (_rtprelay) _rtprelay->start();
}
void Core::rtprelay_stop() {
  if (_rtprelay) _rtprelay->stop();
}

void Core::admin_register(std::shared_ptr<api::AdminAPI> adminAPI) { _adminAPI = adminAPI; }

void Core::admin_start() {
  if (_adminAPI) _adminAPI->start();
}

void Core::admin_stop() {
  if (_adminAPI) _adminAPI->stop();
}

std::shared_ptr<RTPRelaySet> Core::rtprelay_allocate() {
  if (!_rtprelay) return nullptr;
  return _rtprelay->allocate_relay_set();
}

void Core::rtprelay_release(std::shared_ptr<RTPRelaySet> relay) {
  if (!_rtprelay) return;
  _rtprelay->release_relay_set(relay);
}

}  // namespace athenasip
