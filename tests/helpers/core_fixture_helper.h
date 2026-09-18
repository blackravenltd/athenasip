//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "channel.h"
#include "core.h"
#include "datastores/memory_datastore.h"
#include "events/local_event_system.h"
#include "headers/uint_header.h"
#include "timer_source.h"

#include "../mocks/connection_mock.h"
#include "../mocks/logger_mock.h"

// A Core with a memory datastore, a local event system and a manual clock, driven the
// way a transport drives it: raw SIP in through a Channel, raw SIP out through the
// connection underneath it. Nothing here reaches past process_message, so the tests
// exercise the transaction layer and the transaction users together, as a node does.
struct CoreFixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<athenasip::Config> config;
  std::shared_ptr<athenasip::datastores::MemoryDatastore> datastore;
  std::shared_ptr<athenasip::events::LocalEventSystem> event_system;
  std::shared_ptr<athenasip::ManualTimerSource> timers = std::make_shared<athenasip::ManualTimerSource>();
  std::shared_ptr<athenasip::Core> core;

  CoreFixture() {
    config = std::make_shared<athenasip::Config>(logger);
    config->sip_node_id = "test-node";

    datastore = std::make_shared<athenasip::datastores::MemoryDatastore>(logger, std::make_shared<athenasip::types::URL>("memory://"));
    datastore->connect();

    event_system = std::make_shared<athenasip::events::LocalEventSystem>(logger);
    event_system->connect();

    core = std::make_shared<athenasip::Core>(logger, config, datastore, event_system);
    core->timer_source_set(timers);
  }

  // Core's registries are strand-confined, so a test reaches them the way a server does.
  template <typename Fn>
  auto on_strand(Fn&& fn) {
    return core->call_on_strand(std::forward<Fn>(fn));
  }

  std::shared_ptr<athenasip::Channel> make_channel(const std::string& remote_address, std::shared_ptr<MockConnection>* out = nullptr,
                                                  const std::string& transport = "udp") {
    auto connection = std::make_shared<MockConnection>(transport, remote_address);
    if (out) *out = connection;

    auto channel = std::make_shared<athenasip::Channel>(logger, core, connection);
    on_strand([&channel]() { channel->start(); });
    return channel;
  }

  std::shared_ptr<athenasip::types::Realm> seed_realm(const std::string& name) {
    auto realm = std::make_shared<athenasip::types::Realm>(name);
    realm->id = 1;
    realm->nonce_secret = "secret";
    realm->registration_timeout = 3600;
    datastore->realm_create(realm);
    return realm;
  }

  std::shared_ptr<athenasip::types::Subscriber> seed_subscriber(std::uint64_t id, const std::string& uri, const std::string& ha1) {
    auto subscriber = std::make_shared<athenasip::types::Subscriber>();
    subscriber->id = id;
    subscriber->identity = std::make_shared<athenasip::types::SIPIdentity>(uri);
    subscriber->ha1 = ha1;
    datastore->subscriber_create(subscriber);
    return subscriber;
  }

  // Raw SIP in, exactly as the read loop hands it over.
  void receive(const std::shared_ptr<athenasip::Channel>& channel, const std::string& raw) {
    const auto split = raw.find("\r\n\r\n");

    auto message = std::make_shared<athenasip::SIPMessage>();
    message->header = std::make_shared<athenasip::SIPHeader>(split == std::string::npos ? raw : raw.substr(0, split));

    if (split != std::string::npos) {
      message->body = raw.substr(split + 4);
      message->body_length = static_cast<unsigned int>(message->body.size());
    }

    on_strand([&]() { channel->receive(message); });
  }

  // Raw SIP out, parsed back. Content-Length is what separates one message from the next,
  // which is the same rule the far end applies.
  static std::vector<std::shared_ptr<athenasip::SIPMessage>> written(const std::shared_ptr<MockConnection>& connection) {
    std::vector<std::shared_ptr<athenasip::SIPMessage>> messages;

    const auto& raw = connection->written;
    std::size_t pos = 0;

    while (pos < raw.size()) {
      const auto end = raw.find("\r\n\r\n", pos);
      if (end == std::string::npos) break;

      auto message = std::make_shared<athenasip::SIPMessage>();
      message->header = std::make_shared<athenasip::SIPHeader>(raw.substr(pos, end - pos));

      std::size_t body_length = 0;
      if (message->header->contains("Content-Length")) {
        auto length = message->header->headers_map["Content-Length"][0]->as<athenasip::headers::UIntHeader>();
        if (length != nullptr) body_length = static_cast<std::size_t>(length->value);
      }

      message->body = raw.substr(end + 4, body_length);
      messages.push_back(message);

      pos = end + 4 + body_length;
    }

    return messages;
  }

  // The first response with this code, or nullptr.
  static std::shared_ptr<athenasip::SIPMessage> response_with(const std::shared_ptr<MockConnection>& connection, int code) {
    for (const auto& message : written(connection)) {
      if (message->header->type == athenasip::SIPHeader::Type::Response && message->header->response_code == code) return message;
    }
    return nullptr;
  }

  // The first request with this method, or nullptr.
  static std::shared_ptr<athenasip::SIPMessage> request_with(const std::shared_ptr<MockConnection>& connection, const std::string& method) {
    for (const auto& message : written(connection)) {
      if (message->header->type == athenasip::SIPHeader::Type::Request && message->header->request_method == method) return message;
    }
    return nullptr;
  }
};
