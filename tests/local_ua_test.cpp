//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "local_ua.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "helpers/proxy_fixture_helper.h"

using namespace athenasip;

namespace {

struct HangUpFixture : ProxyFixture {
  HangUpFixture() { bind_bob(); }

  // Alice calls Bob and Bob answers; Alice's ACK confirms it.
  void connect_call() {
    receive(caller, invite());
    receive(callee, response_from_callee(200, "OK"));
    settle();
  }

  bool hang_up(const std::string& call_id = "call-proxy") {
    return on_strand([this, call_id]() { return core->local_ua()->hang_up(call_id, "test"); });
  }

  std::shared_ptr<SIPMessage> bye_to(const std::shared_ptr<MockConnection>& connection) {
    const auto byes = requests_with(connection, "BYE");
    return byes.empty() ? nullptr : byes.back();
  }

  // The answer to a BYE, from the end it reached: its Via chain back, and the tags as received.
  std::string ok_for(const std::shared_ptr<SIPMessage>& bye) {
    std::string raw = "SIP/2.0 200 OK\r\n";
    for (const auto& via : bye->header->headers_map["Via"]) raw += "Via: " + via->to_string() + "\r\n";
    raw += "From: " + bye->header->headers_map["From"][0]->to_string() + "\r\n";
    raw += "To: " + bye->header->headers_map["To"][0]->to_string() + "\r\n";
    raw += "Call-ID: " + value_of(bye, "Call-ID") + "\r\n";
    raw += "CSeq: " + bye->header->headers_map["CSeq"][0]->to_string() + "\r\n";
    raw += "\r\n";
    return raw;
  }

  static std::string value_of(const std::shared_ptr<SIPMessage>& message, const std::string& name) {
    return message->header->contains(name) ? message->header->headers_map[name][0]->to_string() : "";
  }

  static std::string tag(const std::shared_ptr<SIPMessage>& message, const std::string& name) {
    const auto value = value_of(message, name);
    const auto at = value.find(";tag=");
    if (at == std::string::npos) return "";
    const auto end = value.find(';', at + 5);
    return value.substr(at + 5, end == std::string::npos ? std::string::npos : end - at - 5);
  }
};

}  // namespace

// RFC 3261 15.1.1 and 12.2.1.1: the BYE to the callee is the one the caller would send - the callee's remote
// target as Request-URI, the caller's tag in From, the callee's in To, and a CSeq above the caller's.
TEST(LocalUATest, TheCalleeIsSentTheByeTheCallerWouldSend) {
  HangUpFixture f;
  f.connect_call();
  ASSERT_EQ(f.dialogs().size(), 1u);

  EXPECT_TRUE(f.hang_up());
  f.settle();

  auto bye = f.bye_to(f.callee_connection);
  ASSERT_NE(bye, nullptr);

  EXPECT_EQ(bye->header->request_uri->to_string(), "sip:bob@192.0.2.20:5060");
  EXPECT_EQ(HangUpFixture::tag(bye, "From"), "alice");
  EXPECT_EQ(HangUpFixture::tag(bye, "To"), "bob");
  EXPECT_EQ(HangUpFixture::value_of(bye, "Call-ID"), "call-proxy");
  EXPECT_EQ(HangUpFixture::value_of(bye, "CSeq"), "2 BYE");
  EXPECT_FALSE(HangUpFixture::value_of(bye, "Max-Forwards").empty());
}

// The other way: the caller is sent the BYE the callee would send. The callee has sent no request, so any CSeq
// is in order (12.2.2).
TEST(LocalUATest, TheCallerIsSentTheByeTheCalleeWouldSend) {
  HangUpFixture f;
  f.connect_call();

  EXPECT_TRUE(f.hang_up());
  f.settle();

  auto bye = f.bye_to(f.caller_connection);
  ASSERT_NE(bye, nullptr);

  EXPECT_EQ(bye->header->request_uri->to_string(), "sip:alice@192.0.2.10:5060");
  EXPECT_EQ(HangUpFixture::tag(bye, "From"), "bob");
  EXPECT_EQ(HangUpFixture::tag(bye, "To"), "alice");
  EXPECT_EQ(HangUpFixture::value_of(bye, "Call-ID"), "call-proxy");
  EXPECT_NE(HangUpFixture::value_of(bye, "CSeq").find("BYE"), std::string::npos);
}

// 15.1.2: a BYE ends the dialog. The node's record of it goes, as for a BYE an end sent.
TEST(LocalUATest, HangingUpEndsTheDialog) {
  HangUpFixture f;
  f.connect_call();

  EXPECT_TRUE(f.hang_up());
  f.settle();

  EXPECT_EQ(f.dialogs().size(), 0u);
}

// The answers come back to this node, which sent the requests, and go no further.
TEST(LocalUATest, TheAnswersAreNotForwardedToEitherEnd) {
  HangUpFixture f;
  f.connect_call();
  EXPECT_TRUE(f.hang_up());
  f.settle();

  auto to_callee = f.bye_to(f.callee_connection);
  auto to_caller = f.bye_to(f.caller_connection);
  ASSERT_NE(to_callee, nullptr);
  ASSERT_NE(to_caller, nullptr);

  const auto caller_before = CoreFixture::written(f.caller_connection).size();
  const auto callee_before = CoreFixture::written(f.callee_connection).size();

  f.receive(f.callee, f.ok_for(to_callee));
  f.receive(f.caller, f.ok_for(to_caller));
  f.settle();

  EXPECT_EQ(CoreFixture::written(f.caller_connection).size(), caller_before);
  EXPECT_EQ(CoreFixture::written(f.callee_connection).size(), callee_before);
}

TEST(LocalUATest, ACallThisNodeDoesNotHoldIsNotHungUp) {
  HangUpFixture f;
  f.connect_call();

  EXPECT_FALSE(f.hang_up("not-a-call"));
  f.settle();

  EXPECT_EQ(f.bye_to(f.callee_connection), nullptr);
  EXPECT_EQ(f.dialogs().size(), 1u);
}

// A call still ringing has no dialog to send a BYE in (15: BYE is for confirmed dialogs); a proxy ends that with
// a CANCEL, which is the caller's to send.
TEST(LocalUATest, AnUnansweredCallIsNotSentABye) {
  HangUpFixture f;
  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(180, "Ringing"));
  f.settle();

  EXPECT_FALSE(f.hang_up());
  f.settle();

  EXPECT_EQ(f.bye_to(f.callee_connection), nullptr);
  EXPECT_EQ(f.bye_to(f.caller_connection), nullptr);
}

// A browser's Contact names nothing reachable (RFC 7118 5.2). The BYE reaches it down its flow, as any in-dialog
// request does, through the flow token in this node's Record-Route.
TEST(LocalUATest, AnEndReachableOnlyByItsFlowIsSentItsByeThere) {
  ProxyFixture f;
  f.bind_bob("sip:k3j2@df7jal23ls0d.invalid;transport=ws");

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(200, "OK", "bob", "sip:k3j2@df7jal23ls0d.invalid;transport=ws"));
  f.settle();

  EXPECT_TRUE(f.on_strand([&f]() { return f.core->local_ua()->hang_up("call-proxy", "test"); }));
  f.settle();

  const auto byes = ProxyFixture::requests_with(f.callee_connection, "BYE");
  ASSERT_EQ(byes.size(), 1u);
  EXPECT_EQ(byes[0]->header->request_uri->to_string(), "sip:k3j2@df7jal23ls0d.invalid;transport=ws");
}
