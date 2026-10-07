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

// Bob registered twice: a desk phone and a second device, each on its own connection.
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

// RFC 5626: a binding is reached over the flow it registered on. Every INVITE written to
// a connection names, in its Request-URI, the device at the other end of that connection.
TEST(ProxyFlowRoutingTest, EveryBranchGoesDownTheFlowOfTheBindingItIsFor) {
  TwoDeviceFixture f;

  f.receive(f.caller, f.invite());

  // Drive the whole fork so both bindings are tried.
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

  // Both were tried, so the invariant is not satisfied by doing nothing.
  EXPECT_EQ(ProxyFixture::requests_with(f.desk_connection, "INVITE").size(), 1u);
  EXPECT_EQ(ProxyFixture::requests_with(f.mobile_connection, "INVITE").size(), 1u);
}

// The second branch goes to the other device, not back to the first.
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

// A binding whose flow has closed falls back to its Contact; if that fails, the fork
// moves on.
TEST(ProxyFlowRoutingTest, ABindingWhoseFlowHasClosedFallsBackToItsContact) {
  TwoDeviceFixture f;

  f.on_strand([&f]() { f.mobile->close(); });
  f.settle();

  f.receive(f.caller, f.invite());

  // The mobile's branch is not sent down the desk's connection.
  auto to_desk = ProxyFixture::requests_with(f.desk_connection, "INVITE");
  ASSERT_LE(to_desk.size(), 1u);

  for (const auto& attempt : to_desk) {
    ASSERT_NE(attempt->header->request_uri, nullptr);
    EXPECT_EQ(attempt->header->request_uri->host, "192.0.2.30");
  }
}
