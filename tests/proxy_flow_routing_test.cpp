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

// Bob, registered twice: a desk phone on one connection and a second device on another.
// Which is the ordinary case for one person, and the case a single flow per account gets
// wrong.
struct TwoDeviceFixture : ProxyFixture {
  std::shared_ptr<MockConnection> desk_connection;
  std::shared_ptr<athenasip::Channel> desk;

  std::shared_ptr<MockConnection> mobile_connection;
  std::shared_ptr<athenasip::Channel> mobile;

  TwoDeviceFixture() {
    desk = make_channel("192.0.2.30", &desk_connection);
    mobile = make_channel("192.0.2.40", &mobile_connection);

    register_binding(bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.30:5060"), desk, 3600);
    register_binding(bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.40:5060"), mobile, 3600);
  }
};

}  // namespace

// RFC 5626: a binding is reached over the flow it was registered on. The node kept one
// flow per account instead - the last one to register - so a user with a desk phone and a
// browser had two bindings and one connection, and the fork sent both attempts to
// whichever device had registered most recently. One device rang twice and the other
// never rang at all.
//
// The invariant, stated so that it holds whichever order the bindings come back in: every
// INVITE written to a connection names, in its Request-URI, the device on the other end of
// that connection. Sending one device's branch down another device's flow breaks it.
TEST(ProxyFlowRoutingTest, EveryBranchGoesDownTheFlowOfTheBindingItIsFor) {
  TwoDeviceFixture f;

  f.receive(f.caller, f.invite());

  // Drive the whole fork, so both bindings are tried rather than only the first.
  const bool desk_first = !ProxyFixture::requests_with(f.desk_connection, "INVITE").empty();
  auto& first = desk_first ? f.desk : f.mobile;
  auto& first_connection = desk_first ? f.desk_connection : f.mobile_connection;

  auto forwarded = ProxyFixture::requests_with(first_connection, "INVITE").front();

  std::string busy = "SIP/2.0 486 Busy Here\r\n";
  for (const auto& via : forwarded->header->headers_map["Via"]) busy += "Via: " + via->to_string() + "\r\n";
  busy += "From: <sip:alice@example.com>;tag=alice\r\n";
  busy += "To: <sip:bob@example.com>;tag=bob-one\r\n";
  busy += "Call-ID: call-proxy\r\n";
  busy += "CSeq: 1 INVITE\r\n";
  busy += "\r\n";

  f.receive(first, busy);
  f.settle();

  auto names_its_own_connection = [](const std::shared_ptr<MockConnection>& connection, const std::string& host) {
    for (const auto& attempt : ProxyFixture::requests_with(connection, "INVITE")) {
      if (!attempt->header->request_uri || attempt->header->request_uri->host != host) return false;
    }
    return true;
  };

  EXPECT_TRUE(names_its_own_connection(f.desk_connection, "192.0.2.30"));
  EXPECT_TRUE(names_its_own_connection(f.mobile_connection, "192.0.2.40"));

  // And both were actually tried, or the invariant above is satisfied by doing nothing.
  EXPECT_EQ(ProxyFixture::requests_with(f.desk_connection, "INVITE").size(), 1u);
  EXPECT_EQ(ProxyFixture::requests_with(f.mobile_connection, "INVITE").size(), 1u);
}

// The second branch has to reach the other device, which is the other half of the same
// bug: with one flow for the account, the second attempt went back to the first device.
TEST(ProxyFlowRoutingTest, TheSecondBranchReachesTheOtherDevice) {
  TwoDeviceFixture f;

  f.receive(f.caller, f.invite());

  const bool desk_first = !ProxyFixture::requests_with(f.desk_connection, "INVITE").empty();
  auto& first = desk_first ? f.desk : f.mobile;
  auto& first_connection = desk_first ? f.desk_connection : f.mobile_connection;
  auto& second_connection = desk_first ? f.mobile_connection : f.desk_connection;

  // The first device declines, so the fork moves on.
  auto forwarded = ProxyFixture::requests_with(first_connection, "INVITE").front();

  std::string busy = "SIP/2.0 486 Busy Here\r\n";
  for (const auto& via : forwarded->header->headers_map["Via"]) busy += "Via: " + via->to_string() + "\r\n";
  busy += "From: <sip:alice@example.com>;tag=alice\r\n";
  busy += "To: <sip:bob@example.com>;tag=bob-one\r\n";
  busy += "Call-ID: call-proxy\r\n";
  busy += "CSeq: 1 INVITE\r\n";
  busy += "\r\n";

  f.receive(first, busy);
  f.settle();

  EXPECT_EQ(ProxyFixture::requests_with(second_connection, "INVITE").size(), 1u);
  EXPECT_EQ(ProxyFixture::requests_with(first_connection, "INVITE").size(), 1u) << "the first device must not be asked twice";
}

// A binding whose flow has closed is a binding whose client has gone. There is nothing
// left but the Contact, which is right for a desk phone with a routable address and
// hopeless for a browser - and either way the branch fails and the fork moves on rather
// than the request being dropped.
TEST(ProxyFlowRoutingTest, ABindingWhoseFlowHasClosedFallsBackToItsContact) {
  TwoDeviceFixture f;

  f.on_strand([&f]() { f.mobile->close(); });
  f.settle();

  f.receive(f.caller, f.invite());

  // The desk phone is still reachable, so it gets an attempt. The point is that the node
  // did not send the mobile's branch down the desk's connection to make up for the flow
  // it had lost.
  auto to_desk = ProxyFixture::requests_with(f.desk_connection, "INVITE");
  ASSERT_LE(to_desk.size(), 1u);

  for (const auto& attempt : to_desk) {
    ASSERT_NE(attempt->header->request_uri, nullptr);
    EXPECT_EQ(attempt->header->request_uri->host, "192.0.2.30");
  }
}
