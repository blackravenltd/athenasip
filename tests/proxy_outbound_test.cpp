//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "helpers/proxy_fixture_helper.h"

using namespace athenasip;

namespace {

const std::string kInstance = "<urn:uuid:00000000-0000-1000-8000-000A95A0E128>";

// Bob is one client holding two RFC 5626 outbound flows, reg-id 1 and 2, each on its own
// TCP connection.
struct OutboundFixture : ProxyFixture {
  std::shared_ptr<MockConnection> first_connection;
  std::shared_ptr<Channel> first;
  std::shared_ptr<MockConnection> second_connection;
  std::shared_ptr<Channel> second;

  OutboundFixture() {
    first = make_channel("192.0.2.20", &first_connection, "tcp", 40001);
    second = make_channel("192.0.2.20", &second_connection, "tcp", 40002);
    register_outbound(bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.20:40001;transport=tcp"), first, kInstance, 1);
    register_outbound(bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.20:40002;transport=tcp"), second, kInstance, 2);
  }

  std::string answer_on(const std::shared_ptr<MockConnection>& connection, int code, const std::string& reason) {
    auto forwarded = requests_with(connection, "INVITE");
    if (forwarded.empty()) return "";

    std::string raw = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n";
    for (const auto& via : forwarded.back()->header->headers_map["Via"]) raw += "Via: " + via->to_string() + "\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:bob@example.com>;tag=bob\r\n";
    raw += "Call-ID: call-proxy\r\n";
    raw += "CSeq: 1 INVITE\r\n";
    raw += "\r\n";
    return raw;
  }

  std::size_t invites_on(const std::shared_ptr<MockConnection>& connection) { return requests_with(connection, "INVITE").size(); }
};

}  // namespace

// RFC 5626 5.3: the target set holds at most one contact per AOR and instance-id, so a
// client with two flows rings once.
TEST(ProxyOutboundTest, OneClientIsReachedDownOneFlowAtATime) {
  OutboundFixture f;

  f.receive(f.caller, f.invite());

  EXPECT_EQ(f.invites_on(f.first_connection) + f.invites_on(f.second_connection), 1u);
}

// RFC 5626 5.3: a 430 Flow Failed moves on to the same client's other flow.
TEST(ProxyOutboundTest, AFailedFlowIsReplacedByTheClientsOtherFlow) {
  OutboundFixture f;

  f.receive(f.caller, f.invite());
  const bool first_tried = f.invites_on(f.first_connection) == 1;
  auto tried = first_tried ? f.first : f.second;
  auto tried_connection = first_tried ? f.first_connection : f.second_connection;
  auto other_connection = first_tried ? f.second_connection : f.first_connection;

  f.receive(tried, f.answer_on(tried_connection, 430, "Flow Failed"));

  EXPECT_EQ(f.invites_on(other_connection), 1u);
  EXPECT_EQ(f.response_with(f.caller_connection, 430), nullptr) << "430 is between proxies, never the caller's";
}

// RFC 5626 5.3: any final response other than 408 or 430 is the client's answer, and its
// other flow is not tried.
TEST(ProxyOutboundTest, AnyOtherAnswerIsTheClientsAnswer) {
  OutboundFixture f;

  f.receive(f.caller, f.invite());
  const bool first_tried = f.invites_on(f.first_connection) == 1;
  auto tried = first_tried ? f.first : f.second;
  auto tried_connection = first_tried ? f.first_connection : f.second_connection;
  auto other_connection = first_tried ? f.second_connection : f.first_connection;

  f.receive(tried, f.answer_on(tried_connection, 486, "Busy Here"));

  EXPECT_EQ(f.invites_on(other_connection), 0u);
  EXPECT_NE(f.response_with(f.caller_connection, 486), nullptr);
}

// RFC 5626 5.3: an outbound binding is reached only down its flow, never at its Contact.
// A closed flow is a failed flow and the client's other flow is used.
TEST(ProxyOutboundTest, AClosedFlowIsNotReplacedByItsContact) {
  OutboundFixture f;

  f.on_strand([&f]() {
    f.first->close();
    f.second->close();
  });
  f.settle();

  std::shared_ptr<MockConnection> third_connection;
  auto third = f.make_channel("192.0.2.20", &third_connection, "tcp", 40003);
  f.register_outbound(f.bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.20:40003;transport=tcp"), third, kInstance, 3);

  // Only the live flow is reached, and no connection is opened to a dead flow's Contact.
  f.receive(f.caller, f.invite());

  EXPECT_EQ(f.invites_on(third_connection), 1u);
}
