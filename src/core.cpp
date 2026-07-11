//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "core.h"
#include "channel.h"
#include "transaction.h"
#include "rtp/rtp_relay.h"
#include "expiry_set.h"

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
      _nonce_cache(std::make_shared<ExpirySet<std::string>>()) {}

void Core::server_register(std::shared_ptr<servers::Server> server) { _servers.push_back(server); }

void Core::server_start_all() {
  for (auto& server : _servers) server->start();
}

void Core::server_stop_all() {
  for (auto& server : _servers) server->stop();
}

// Realms
std::shared_ptr<Realm> Core::realm_get_by_name(const std::string& realm_name) {
  return datastore->realm_get_by_name(realm_name);
};

// Subscribers
std::shared_ptr<Subscriber> Core::subscriber_get(std::shared_ptr<SIPIdentity> identity) {
  return datastore->subscriber_get(identity);
}

bool Core::subscriber_register(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel) {

  auto registration = subscriber_get(subscriber->identity);

  if(!registration && !datastore->subscriber_register(subscriber, contact)) {
    _logger->error("Cannot register subscriber identity "+ subscriber->identity->to_string()+" - datastore failure");
    return false;
  } 

  if(!channel->_event_subscription) {
    channel->_event_subscription = events->subscribe(config->sip_event_prefix+"/subscriber/"+subscriber->identity->uri->to_string()+"/#", 
      [this, subscriber](std::string event, std::string payload) {
        _logger->info("------------------------------- SUBSCRIBER "+subscriber->identity->to_string()+" Event: "+event+" Payload: "+payload);
      }
    );
  }

  _channels_by_subscriber[subscriber->id] = channel;
  events->publish(config->sip_event_prefix+"/subscriber/"+subscriber->identity->uri->to_string()+"/status","{\"contact\":\""+contact->to_string()+"\",\"node\":\""+config->sip_node_id+"\",\"registered\":\""+Util::get_zulu_time()+"\"}");
  return true;
}

bool Core::subscriber_unregister(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel) {
  if(datastore->subscriber_unregister(subscriber, contact)) {
    _channels_by_subscriber.erase(subscriber->id);
    return true;
  }

  if(channel->_event_subscription) {
    events->unsubscribe(channel->_event_subscription);
    channel->_event_subscription = nullptr;
  }

    _logger->error("Cannot unregister subscriber identity "+ subscriber->identity->to_string()+" - datastore failure");
  return false;
}

std::shared_ptr<Channel> Core::subscriber_get_channel(std::shared_ptr<Subscriber> subscriber) { return _channels_by_subscriber[subscriber->id]; }

// Channels

bool Core::channel_register(std::string endpoint, std::shared_ptr<Channel> channel) {
  std::lock_guard<std::shared_mutex> lock(_channels_mutex);
  _channels[endpoint] = channel;

  events->publish(config->sip_event_prefix+"/nodes/"+config->sip_node_id+"/channels/"+channel->_connection->remote_endpoint_name()+";transport="+channel->_connection->transport_name(),"{\"status\":\"registered\",\"at\":\""+Util::get_zulu_time()+"\"}");

  _logger->debug("Registered Channel " + endpoint);
  return true;
}

bool Core::channel_unregister(std::string endpoint, std::shared_ptr<Channel> channel) {
  std::lock_guard<std::shared_mutex> lock(_channels_mutex);
  events->publish(config->sip_event_prefix+"/nodes/"+config->sip_node_id+"/channels/"+channel->_connection->remote_endpoint_name()+";transport="+channel->_connection->transport_name(),"{\"status\":\"closed\",\"at\":\""+Util::get_zulu_time()+"\"}");

  _channels.erase(endpoint);  
  _logger->debug("Unregistered Channel " + endpoint);
  return true;
}

void Core::channel_close_all() {
  // Close All Channels
  for (const auto& pair : _channels) pair.second->close();

  // Remove all Channels
  std::unique_lock<std::shared_mutex> lock(_channels_mutex);
  _channels.clear();
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

  if(datastore->nonce_create(nonce, expires_at)) {
    // Cache the actual nonce for this node
    _nonce_cache->add(nonce, realm->nonce_expiry * 1000);
    return nonce;
  }

  throw std::runtime_error("Failed to generate nonce - datastore error");
}

bool Core::nonce_check(std::string nonce) {
  return datastore->nonce_check(nonce);
}

// Messages

void Core::process_message(std::shared_ptr<SIPMessage> message) {
  // Message must contain Via and CSeq to identify the transaction
  if (!message->header->contains("Via") || !message->header->contains("CSeq")) {
    _logger->info("[Request] - Incomplete Headers (No Via/CSeq) - Sending 400 Bad Request");

    // Send 400 Bad Request
    auto response = message->generate_response();
    response->header->add("Reason", "SIP ;cause=400 ;text=\"Incomplete Headers (Needs From, To, Call-ID, CSeq, Via, Max-Forwards)\"");
    response->header->response_code = 400;
    response->header->response_message = "Bad Request";
    response->channel->send(response);

    return;
  }

  // Find or create the message transaction
  auto transactionId = message->get_transaction_id();
  _logger->debug("[Request] - Transaction is " + transactionId);
  message->transaction = transaction_get(transactionId);
  if (!message->transaction) {
    message->transaction = std::make_shared<Transaction>(_logger, message->channel, shared_from_this(), Transaction::Direction::Incoming, transactionId);
    message->transaction->type = (message->header->type == SIPHeader::Type::Request && message->header->request_method == "INVITE")
                                     ? Transaction::Type::INVITE
                                     : Transaction::Type::NonINVITE;
    message->transaction->start(config->sip_timer_t1_rtt_ms);
  } else {
    message->transaction->reset_timers();
  }

  // Parse the Message in the context of the transaction
  message->transaction->receive_message(message);
}

// Transactions

bool Core::transaction_register(std::shared_ptr<Transaction> transaction) {
  std::unique_lock<std::shared_mutex> lock(_transactions_mutex);

  _transactions[transaction->id] = transaction;
  events->publish("transaction.register", transaction->id);
  return true;
}

bool Core::transaction_unregister(std::string transactionId) {
  auto transaction = transaction_get(transactionId);

  if (!transaction) return false;

  std::unique_lock<std::shared_mutex> lock(_transactions_mutex);
  _transactions.erase(transactionId);
  events->publish("transaction.unregister", transactionId);
  return true;
}

std::shared_ptr<Transaction> Core::transaction_get(std::string transactionId) {
  std::unique_lock<std::shared_mutex> lock(_transactions_mutex);

  auto search = _transactions.find(transactionId);
  if (search == _transactions.end()) return nullptr;
  return search->second;
}

void Core::transaction_end_all() {
  // End all transactions
  for (const auto& pair : _transactions) pair.second->end();

  // Remove all transactions
  std::unique_lock<std::shared_mutex> lock(_transactions_mutex);
  _transactions.clear();
}

// Calls
bool Core::call_register(std::shared_ptr<Call> call) {
  _calls[call->id] = call;
  events->publish("call.register", call->id);
  return true;
}

bool Core::call_unregister(std::string callId) {
  _calls.erase(callId);
  events->publish("call.unregister", callId);
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
