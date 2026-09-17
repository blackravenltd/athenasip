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
#include "rtp/rtp_relay.h"
#include "transaction.h"
#include "types/sip_uri.h"

using namespace athenasip::servers;
using namespace athenasip::datastores;
using namespace athenasip::events;
using namespace athenasip::rtp;
using namespace athenasip::types;

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
std::shared_ptr<Realm> Core::realm_get_by_name(const std::string& realm_name) { return datastore->realm_get_by_name(realm_name); };

// Subscribers
std::shared_ptr<Subscriber> Core::subscriber_get(std::shared_ptr<SIPIdentity> identity) { return datastore->subscriber_get(identity); }

bool Core::subscriber_register(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel) {
  // RFC 3261 10.3 step 7: the binding is written on every successful REGISTER. This
  // used to be skipped whenever the subscriber record already existed, which is always,
  // so no contact was ever stored and the registrar had nothing to route to.
  if (!datastore->subscriber_register(subscriber, contact)) {
    _logger->error("Cannot register subscriber identity " + subscriber->identity->to_string() + " - datastore failure");
    return false;
  }

  if (!channel->_event_subscription) {
    // Subscribe to the invite topic alone. A "subscriber/<uri>/#" filter also matches
    // this node's own status publish below, and that payload has no call fields.
    std::weak_ptr<Core> weak_core = weak_from_this();
    std::weak_ptr<Channel> weak_channel = channel;
    auto identity = subscriber->identity;

    channel->_event_subscription = events->subscribe(events::topics::subscriber_invite(identity->uri->to_string()),
                                                     [weak_core, weak_channel, identity](std::string event, std::string payload) {
                                                       auto core = weak_core.lock();
                                                       auto event_channel = weak_channel.lock();

                                                       // Either the node or the channel went away between publish and delivery.
                                                       if (!core || !event_channel) return;

                                                       core->_invite_from_event(event_channel, identity, event, payload);
                                                     });
  }

  _channels_by_subscriber[subscriber->id] = channel;

  events->publish(events::topics::subscriber_status(subscriber->identity->uri->to_string()),
                  "{\"contact\":\"" + contact->to_string() + "\",\"node\":\"" + config->sip_node_id + "\",\"registered\":\"" + Util::get_zulu_time() + "\"}");
  return true;
}

// Turns a subscriber/<uri>/invite event into an outbound INVITE. The payload comes off
// the event bus, so every field is checked before it is used.
void Core::_invite_from_event(std::shared_ptr<Channel> channel, std::shared_ptr<SIPIdentity> identity, const std::string& event, const std::string& payload) {
  _logger->info("Received Event for: " + identity->to_string() + " Event: " + event);

  boost::system::error_code ec;
  auto parsed = boost::json::parse(payload, ec);

  if (ec || !parsed.is_object()) {
    _logger->error("Ignoring event " + event + " - payload is not a JSON object");
    return;
  }

  const auto& payload_obj = parsed.as_object();

  for (const auto* field : {"call_id", "from", "to", "sdp"}) {
    if (!payload_obj.contains(field) || !payload_obj.at(field).is_string()) {
      _logger->error("Ignoring event " + event + " - missing or non-string field " + field);
      return;
    }
  }

  auto transaction =
      std::make_shared<Transaction>(_logger, channel, shared_from_this(), Transaction::Direction::Outgoing, Util::generate_random_string("", 10));
  transaction_register(transaction);

  auto invite = std::make_shared<SIPMessage>();

  invite->header = std::make_shared<SIPHeader>();
  invite->header->request_method = "INVITE";

  // Without a request URI the header cannot serialise. Target the callee for now; the
  // M2 proxy core replaces this with a location lookup.
  auto to_identity = std::make_shared<SIPIdentity>(std::string(payload_obj.at("to").as_string()));
  invite->header->request_uri = to_identity->uri;

  invite->header->add("Call-ID", std::string(payload_obj.at("call_id").as_string()));
  invite->header->add("From", std::string(payload_obj.at("from").as_string()));
  invite->header->add("To", std::string(payload_obj.at("to").as_string()));
  invite->header->add("Content-Type", "application/sdp");
  invite->body = std::string(payload_obj.at("sdp").as_string());

  transaction->send_message(invite);
}

bool Core::subscriber_unregister(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel) {
  // The subscription is per-channel state and the caller is done with this subscriber
  // on this channel either way, so it goes before the datastore call rather than only
  // on the failure path, where it used to sit and therefore leaked on every success.
  if (channel && channel->_event_subscription) {
    events->unsubscribe(channel->_event_subscription);
    channel->_event_subscription = nullptr;
  }

  _channels_by_subscriber.erase(subscriber->id);

  if (!datastore->subscriber_unregister(subscriber, contact)) {
    _logger->error("Cannot unregister subscriber identity " + subscriber->identity->to_string() + " - datastore failure");
    return false;
  }

  return true;
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

std::string Core::nonce_create(std::shared_ptr<Realm> realm) {
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

  if (datastore->nonce_create(nonce, expires_at)) {
    // Cache the actual nonce for this node
    _nonce_cache->add(nonce, realm->nonce_expiry * 1000);
    return nonce;
  }

  throw std::runtime_error("Failed to generate nonce - datastore error");
}

bool Core::nonce_check(std::string nonce) { return datastore->nonce_check(nonce); }

// Messages

void Core::process_message(std::shared_ptr<SIPMessage> message) {
  // Message must contain Via and CSeq to identify the transaction
  if (!message->header->contains("Via") || !message->header->contains("CSeq")) {
    _logger->info("[Request] - Incomplete Headers (No Via/CSeq) - Sending 400 Bad Request");

    if (message->channel.expired()) {
      _logger->info("[Request] - Channel has closed, cannot respond");
    }

    // Send 400 Bad Request
    auto response = message->generate_response();
    response->header->add("Reason", "SIP ;cause=400 ;text=\"Incomplete Headers (Needs From, To, Call-ID, CSeq, Via, Max-Forwards)\"");
    response->header->response_code = 400;
    response->header->response_message = "Bad Request";
    response->channel.lock()->send(response);

    return;
  }

  // Find or create the message transaction
  auto transactionId = message->get_transaction_id();
  _logger->debug("[Request] - Transaction is " + transactionId);
  message->transaction = transaction_get(transactionId);
  std::shared_ptr<Transaction> transaction = message->transaction.lock();

  if (!transaction) {
    transaction = std::make_shared<Transaction>(_logger, message->channel.lock(), shared_from_this(), Transaction::Direction::Incoming, transactionId);
    transaction->type = (message->header->type == SIPHeader::Type::Request && message->header->request_method == "INVITE") ? Transaction::Type::INVITE
                                                                                                                           : Transaction::Type::NonINVITE;
    transaction_register(transaction);
    transaction->start(config->sip_timer_t1_rtt_ms);
    message->transaction = transaction;
  } else {
    transaction->reset_timers();
  }

  // Parse the Message in the context of the transaction
  transaction->receive_message(message);
}

// Transactions

bool Core::transaction_register(std::shared_ptr<Transaction> transaction) {
  _transactions[transaction->id] = transaction;
  events->publish(events::topics::node_transaction(config->sip_node_id, transaction->id), "registered");
  return true;
}

bool Core::transaction_unregister(std::string transactionId) {
  auto transaction = transaction_get(transactionId);

  if (!transaction) return false;

  _transactions.erase(transactionId);
  events->publish(events::topics::node_transaction(config->sip_node_id, transaction->id), "unregistered");
  return true;
}

std::shared_ptr<Transaction> Core::transaction_get(std::string transactionId) {
  auto search = _transactions.find(transactionId);
  if (search == _transactions.end()) return nullptr;
  return search->second;
}

void Core::transaction_end_all() {
  // end() unregisters, which erases from _transactions. Take a copy and empty the map
  // first so nothing mutates it while we are walking it.
  std::vector<std::shared_ptr<Transaction>> transactions;
  transactions.reserve(_transactions.size());
  for (const auto& [id, transaction] : _transactions) transactions.push_back(transaction);
  _transactions.clear();

  for (const auto& transaction : transactions) transaction->end();
}

// Calls
bool Core::call_register(std::shared_ptr<Call> call) {
  _calls[call->id] = call;

  datastore->call_create(call);

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
