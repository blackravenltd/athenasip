//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// A dialog's state is in the call record, so a request inside it reaching a node that does not hold the dialog
// finds it there (all state lives in the datastore, 2026-10-11).
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "call.h"
#include "config.h"
#include "core.h"
#include "helpers/proxy_fixture_helper.h"
#include "mocks/connection_mock.h"

using namespace athenasip;

namespace {

// The fixture's node, and a second with the same name and datastore: the node after a restart, holding nothing.
struct RestartedFixture : ProxyFixture {
  std::shared_ptr<Config> config_b;
  std::shared_ptr<Core> node_b;

  RestartedFixture() {
    bind_bob();
    receive(caller, invite());
    receive(callee, response_from_callee(200, "OK"));
    settle();

    config_b = std::make_shared<Config>(logger);
    config_b->sip_node_id = config->sip_node_id;
    node_b = std::make_shared<Core>(logger, config_b, datastore, event_system);
    node_b->timer_source_set(timers);
  }

  ~RestartedFixture() { settle_b(); }

  void settle_b(int rounds = 32) {
    for (int i = 0; i < rounds; ++i) node_b->call_on_strand([]() {});
  }

  // A message arriving at node B over a connection from the given address.
  std::shared_ptr<MockConnection> arrive_b(const std::string& from_address, const std::string& raw) {
    auto connection = std::make_shared<MockConnection>("udp", from_address, 5060);
    auto channel = std::make_shared<Channel>(logger, node_b, connection);
    node_b->call_on_strand([&channel]() { channel->start(); });

    const auto split = raw.find("\r\n\r\n");
    auto message = std::make_shared<SIPMessage>();
    message->header = std::make_shared<SIPHeader>(split == std::string::npos ? raw : raw.substr(0, split));
    if (split != std::string::npos) {
      message->body = raw.substr(split + 4);
      message->body_length = static_cast<unsigned int>(message->body.size());
    }
    node_b->call_on_strand([&]() { channel->receive(message); });
    settle_b();
    return connection;
  }

  static std::string bye_from_callee() {
    std::string raw = "BYE sip:alice@192.0.2.10:5060 SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.20:5060;branch=z9hG4bK-bye-b\r\n";
    raw += "Route: <sip:192.0.2.1:5060;lr>\r\n";
    raw += "From: <sip:bob@example.com>;tag=bob\r\n";
    raw += "To: <sip:alice@example.com>;tag=alice\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 2 BYE\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "\r\n";
    return raw;
  }
};

}  // namespace

// The restarted node holds no dialog. Bob's BYE reaches it, it reads the call record, carries the dialog on, and
// so sees the call end: the record closes and the line is free, as it would have been on the node that began it.
TEST(CoreDialogRecordTest, ARequestInADialogTheNodeDoesNotHoldFindsItInTheCallRecord) {
  RestartedFixture f;
  ASSERT_EQ(f.store->call_get("call-proxy")->state, Call::State::Connected);
  EXPECT_EQ(f.node_b->call_on_strand([&f]() { return f.node_b->dialogs()->size(); }), 0u);

  f.arrive_b("192.0.2.20", RestartedFixture::bye_from_callee());

  bool taken = false;
  for (const auto& line : f.logger->lines(loggers::LogLevel::INFO)) taken = taken || line.find("taken from its record") != std::string::npos;
  EXPECT_TRUE(taken);
  EXPECT_EQ(f.store->call_get("call-proxy")->state, Call::State::Closed);
}

// A request in no dialog any record describes is handled as before: nothing is taken in.
TEST(CoreDialogRecordTest, ARequestInADialogNoRecordDescribesIsNotTakenIn) {
  RestartedFixture f;

  std::string raw = "BYE sip:alice@192.0.2.10:5060 SIP/2.0\r\n";
  raw += "Via: SIP/2.0/UDP 192.0.2.20:5060;branch=z9hG4bK-bye-x\r\n";
  raw += "From: <sip:bob@example.com>;tag=bob\r\n";
  raw += "To: <sip:alice@example.com>;tag=alice\r\n";
  raw += "Call-ID: call-unknown\r\n";
  raw += "CSeq: 2 BYE\r\n";
  raw += "Max-Forwards: 70\r\n\r\n";
  f.arrive_b("192.0.2.20", raw);

  EXPECT_EQ(f.node_b->call_on_strand([&f]() { return f.node_b->dialogs()->size(); }), 0u);
  EXPECT_EQ(f.store->call_get("call-proxy")->state, Call::State::Connected);
}
