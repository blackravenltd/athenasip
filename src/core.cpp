//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "core.h"

#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <chrono>

#include "channel.h"
#include "events/topics.h"
#include "expiry_set.h"
#include "proxy.h"
#include "registrar.h"
#include "rtp/rtp_relay.h"
#include "servers/tcp_connection.h"
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

// Accounts
void Core::account_get(std::shared_ptr<SIPIdentity> identity, plugins::Handler<std::shared_ptr<Account>> handler) {
  datastore->account_get(_strand, std::move(identity), std::move(handler));
}

void Core::location_list(std::uint64_t account_id, plugins::Handler<std::vector<types::Location>> handler) {
  datastore->location_list(_strand, account_id, std::move(handler));
}

void Core::account_register(std::shared_ptr<Account> account, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel, std::uint32_t expires_seconds,
                            std::string path, plugins::StatusHandler handler) {
  // RFC 3261 10.3 step 7: the binding is written on every successful REGISTER. This
  // used to be skipped whenever the account record already existed, which is always,
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
  if (channel) binding.flow_id = channel->flow_id();

  // The channel index and the event both wait for the write: a binding nobody stored is
  // not one to announce.
  datastore->account_register(
      _strand, account, std::move(binding), expires_seconds, [this, account, contact, channel, handler](plugins::Status status) mutable {
        if (!status.ok) {
          _logger->error("Cannot register account identity " + account->identity->to_string() + " - " + status.error);
          if (handler) handler(status);
          return;
        }

        if (channel) _channels_by_account[account->id] = channel;

        events->publish(
            events::topics::account_status(account->identity->uri->to_string()),
            "{\"contact\":\"" + contact->to_string() + "\",\"node\":\"" + config->sip_node_id + "\",\"registered\":\"" + Util::get_zulu_time() + "\"}");

        if (handler) handler(status);
      });
}

void Core::account_unregister(std::shared_ptr<Account> account, std::shared_ptr<SIPUri> contact, std::shared_ptr<Channel> channel,
                              plugins::StatusHandler handler) {
  (void)channel;

  _channels_by_account.erase(account->id);

  auto self = shared_from_this();

  datastore->account_unregister(_strand, account, contact, [this, self, account, handler](plugins::Status status) mutable {
    if (!status.ok) _logger->error("Cannot unregister account identity " + account->identity->to_string() + " - " + status.error);
    if (handler) handler(status);
  });
}

std::shared_ptr<Channel> Core::account_get_channel(std::shared_ptr<Account> account) {
  // operator[] would insert an empty entry for an unknown account.
  auto search = _channels_by_account.find(account->id);
  if (search == _channels_by_account.end()) return nullptr;
  return search->second;
}

// Channels

std::string Core::channel_key(const std::string& transport, const std::string& host, std::uint16_t port) {
  return channel_key(transport, host + ":" + std::to_string(port));
}

std::string Core::channel_key(const std::string& transport, const std::string& endpoint) { return Util::to_lower(transport) + "://" + endpoint; }

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

  _logger->debug("Unregistered Channel " + endpoint + (removed > 1 ? " and " + std::to_string(removed - 1) + " alias(es)" : ""));
  return true;
}

std::shared_ptr<Channel> Core::channel_find(const std::string& transport, const std::string& host, std::uint16_t port) {
  auto search = _channels.find(channel_key(transport, host, port));
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

  // UDP has no connection to open. A datagram to a host this node has never heard from
  // has to leave by the listener's own socket so that the source port is the one the far
  // end will answer to, and that socket belongs to the UDP server rather than to this
  // registry. TLS outbound waits for the trust configuration the cluster CA brings.
  if (transport != "tcp") {
    return handler(ChannelResult::failure("cannot open an outbound " + transport + " flow"));
  }

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
  resolver->async_resolve(host, std::to_string(port), [weak_self, resolver, socket, answer, key](const boost::system::error_code& ec, auto results) {
    if (ec) return answer(ChannelResult::failure("cannot resolve " + key + " - " + ec.message()));

    boost::asio::async_connect(*socket, results, [weak_self, socket, answer, key](const boost::system::error_code& ec, auto) {
      if (ec) return answer(ChannelResult::failure("cannot reach " + key + " - " + ec.message()));

      auto self = weak_self.lock();
      if (!self) return;

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

void Core::_deliver_to_tu(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<transactions::TransactionBase>& transaction) {
  _ensure_transaction_users();

  // Here rather than in process_message, because this is where a request has been
  // de-duplicated: a retransmission is absorbed by its transaction and never arrives
  // (17.2.1), so what the tracker sees is each request once.
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
