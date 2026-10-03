//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "../mocks/logger_mock.h"
#include "headers/cseq_header.h"
#include "transactions/invite_client_transaction.h"
#include "transactions/non_invite_client_transaction.h"

using namespace athenasip;
using namespace athenasip::transactions;
using namespace std::chrono_literals;

namespace {

std::shared_ptr<SIPMessage> request(const std::string& method) {
  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>(method +
                                                " sip:bob@example.com SIP/2.0\r\n"
                                                "Via: SIP/2.0/UDP alice.example.com:5060;branch=z9hG4bK-one\r\n"
                                                "From: <sip:alice@example.com>;tag=alice\r\n"
                                                "To: <sip:bob@example.com>\r\n"
                                                "Call-ID: call-1\r\n"
                                                "CSeq: 314 " +
                                                method +
                                                "\r\n"
                                                "Max-Forwards: 70\r\n"
                                                "\r\n");
  return message;
}

std::shared_ptr<SIPMessage> response_to(const std::shared_ptr<SIPMessage>& to, int code, const std::string& reason) {
  auto message = to->generate_response();
  message->header->response_code = code;
  message->header->response_message = reason;
  return message;
}

struct Harness {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<ManualTimerSource> timers = std::make_shared<ManualTimerSource>();

  std::vector<std::shared_ptr<SIPMessage>> sent;
  std::vector<std::shared_ptr<SIPMessage>> to_tu;
  int timeouts = 0;

  Timers values() const {
    Config config(logger);
    return Timers::from_config(config);
  }

  int sent_method(const std::string& method) const {
    int count = 0;
    for (const auto& message : sent) {
      if (message->header->type == SIPHeader::Type::Request && message->header->request_method == method) count++;
    }
    return count;
  }
};

std::shared_ptr<InviteClientTransaction> make_ict(Harness& h, bool reliable) {
  auto transaction = std::make_shared<InviteClientTransaction>(
      h.logger, "ict-1", reliable, h.values(), h.timers, [&h](std::shared_ptr<SIPMessage> m) { h.sent.push_back(m); },
      [&h](std::shared_ptr<SIPMessage> m) { h.to_tu.push_back(m); });

  transaction->on_timeout([&h]() { h.timeouts++; });
  return transaction;
}

std::shared_ptr<NonInviteClientTransaction> make_nict(Harness& h, bool reliable) {
  auto transaction = std::make_shared<NonInviteClientTransaction>(
      h.logger, "nict-1", reliable, h.values(), h.timers, [&h](std::shared_ptr<SIPMessage> m) { h.sent.push_back(m); },
      [&h](std::shared_ptr<SIPMessage> m) { h.to_tu.push_back(m); });

  transaction->on_timeout([&h]() { h.timeouts++; });
  return transaction;
}

}  // namespace

// ---- INVITE client transaction, RFC 3261 17.1.1 ----

TEST(InviteClientTransactionTest, SendsTheInviteAndEntersCalling) {
  Harness h;
  auto transaction = make_ict(h, false);

  transaction->start(request("INVITE"));

  EXPECT_EQ(transaction->state(), State::Calling);
  EXPECT_EQ(h.sent_method("INVITE"), 1);
}

// Timer A doubles on every retransmission. Unlike E it is not capped at T2, because
// timer B ends the attempt first.
TEST(InviteClientTransactionTest, RetransmitsWithDoublingIntervals) {
  Harness h;
  auto transaction = make_ict(h, false);
  transaction->start(request("INVITE"));

  h.timers->advance(500ms);  // A = T1
  EXPECT_EQ(h.sent_method("INVITE"), 2);

  h.timers->advance(1s);  // 2*T1
  EXPECT_EQ(h.sent_method("INVITE"), 3);

  h.timers->advance(2s);  // 4*T1
  EXPECT_EQ(h.sent_method("INVITE"), 4);

  h.timers->advance(4s);  // 8*T1, still doubling past T2
  EXPECT_EQ(h.sent_method("INVITE"), 5);

  h.timers->advance(8s);  // 16*T1
  EXPECT_EQ(h.sent_method("INVITE"), 6);
}

TEST(InviteClientTransactionTest, DoesNotRetransmitOnAReliableTransport) {
  Harness h;
  auto transaction = make_ict(h, true);
  transaction->start(request("INVITE"));

  h.timers->advance(10s);
  EXPECT_EQ(h.sent_method("INVITE"), 1);
}

TEST(InviteClientTransactionTest, AProvisionalStopsRetransmissions) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ict(h, false);
  transaction->start(invite);

  transaction->receive(response_to(invite, 180, "Ringing"));
  EXPECT_EQ(transaction->state(), State::Proceeding);
  ASSERT_EQ(h.to_tu.size(), 1u);

  h.timers->advance(30s);
  EXPECT_EQ(h.sent_method("INVITE"), 1) << "a provisional must stop timer A";
}

// Timer B: nothing came back at all.
TEST(InviteClientTransactionTest, TimesOutWithNoResponse) {
  Harness h;
  auto transaction = make_ict(h, false);
  transaction->start(request("INVITE"));

  h.timers->advance(31s);
  EXPECT_EQ(transaction->state(), State::Calling);

  h.timers->advance(1s);  // B = 64 * T1 = 32s
  EXPECT_EQ(transaction->state(), State::Terminated);
  EXPECT_EQ(h.timeouts, 1);
}

// RFC 6026 section 7.2, which replaces RFC 3261 17.1.1.2 here: a 2xx moves the
// transaction to Accepted rather than ending it, and is not acknowledged by it - the ACK
// for a 2xx is the TU's, a separate transaction that may take a different route.
TEST(InviteClientTransactionTest, TwoHundredGoesToAcceptedAndIsNotAcknowledgedHere) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ict(h, false);
  transaction->start(invite);

  transaction->receive(response_to(invite, 200, "OK"));

  EXPECT_EQ(transaction->state(), State::Accepted);
  ASSERT_EQ(h.to_tu.size(), 1u);
  EXPECT_EQ(h.sent_method("ACK"), 0) << "the transaction must not ACK a 2xx";
}

// The reason for Accepted: a retransmitted 2xx - the UAS resends it until its ACK arrives
// - still matches this transaction and is passed to the TU every time, so a proxy can send
// it on to the caller. Ended at once, the retransmission had no transaction to match and
// arrived as a stray, which a proxy can only route by Via; a WebSocket caller's Via names
// nothing (RFC 7118), and the 2xx was lost.
TEST(InviteClientTransactionTest, ARetransmittedTwoHundredIsPassedToTheTuEachTime) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ict(h, true);
  transaction->start(invite);

  transaction->receive(response_to(invite, 200, "OK"));
  transaction->receive(response_to(invite, 200, "OK"));

  EXPECT_EQ(h.to_tu.size(), 2u);
  EXPECT_EQ(transaction->state(), State::Accepted);

  // And a non-2xx in Accepted is nothing: the call was answered.
  transaction->receive(response_to(invite, 486, "Busy Here"));
  EXPECT_EQ(h.to_tu.size(), 2u);
}

// Timer M, 64*T1, ends Accepted, on any transport: 2xx retransmissions come from the UAS's
// own timer, not this transport's.
TEST(InviteClientTransactionTest, TimerMEndsAccepted) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ict(h, true);
  transaction->start(invite);
  transaction->receive(response_to(invite, 200, "OK"));

  h.timers->advance(h.values().m - 1ms);
  EXPECT_EQ(transaction->state(), State::Accepted);

  h.timers->advance(1ms);
  EXPECT_EQ(transaction->state(), State::Terminated);
}

// A non-2xx is acknowledged by the transaction itself.
TEST(InviteClientTransactionTest, AcknowledgesANonTwoHundredItself) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ict(h, false);
  transaction->start(invite);

  transaction->receive(response_to(invite, 486, "Busy Here"));

  EXPECT_EQ(transaction->state(), State::Completed);
  EXPECT_EQ(h.sent_method("ACK"), 1);
  ASSERT_EQ(h.to_tu.size(), 1u);
}

// RFC 3261 17.1.1.3 spells out exactly which headers the ACK carries.
TEST(InviteClientTransactionTest, TheAckFollowsTheRulesForItsHeaders) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ict(h, false);
  transaction->start(invite);

  auto busy = response_to(invite, 486, "Busy Here");
  transaction->receive(busy);

  auto ack = transaction->last_ack();
  ASSERT_NE(ack, nullptr);

  EXPECT_EQ(ack->header->request_method, "ACK");
  EXPECT_EQ(ack->header->request_uri->to_string(), invite->header->request_uri->to_string());

  // One Via, equal to the request's topmost.
  ASSERT_EQ(ack->header->headers_map["Via"].size(), 1u);
  EXPECT_EQ(ack->header->headers_map["Via"][0]->to_string(), invite->header->headers_map["Via"][0]->to_string());

  // Call-ID and From come from the request.
  EXPECT_EQ(ack->header->headers_map["Call-ID"][0]->to_string(), "call-1");
  EXPECT_NE(ack->header->headers_map["From"][0]->to_string().find("tag=alice"), std::string::npos);

  // The To comes from the response, because it carries the tag the far end chose.
  EXPECT_EQ(ack->header->headers_map["To"][0]->to_string(), busy->header->headers_map["To"][0]->to_string());

  // Same sequence number, method ACK.
  auto cseq = ack->header->headers_map["CSeq"][0]->as<headers::CSeqHeader>();
  ASSERT_NE(cseq, nullptr);
  EXPECT_EQ(cseq->sequence, 314u);
  EXPECT_EQ(cseq->method, "ACK");
}

// In Completed the ACK is repeated for each retransmitted final response, and the TU
// hears about the failure only once.
TEST(InviteClientTransactionTest, RepeatsTheAckWithoutTellingTheTuAgain) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ict(h, false);
  transaction->start(invite);

  auto busy = response_to(invite, 486, "Busy Here");
  transaction->receive(busy);
  transaction->receive(busy);
  transaction->receive(busy);

  EXPECT_EQ(h.sent_method("ACK"), 3);
  EXPECT_EQ(h.to_tu.size(), 1u);
}

TEST(InviteClientTransactionTest, TimerDEndsTheTransaction) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ict(h, false);
  transaction->start(invite);
  transaction->receive(response_to(invite, 486, "Busy Here"));

  h.timers->advance(31s);
  EXPECT_EQ(transaction->state(), State::Completed);

  h.timers->advance(1s);  // D = 32s
  EXPECT_EQ(transaction->state(), State::Terminated);
}

TEST(InviteClientTransactionTest, CompletedEndsAtOnceOnAReliableTransport) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ict(h, true);
  transaction->start(invite);
  transaction->receive(response_to(invite, 486, "Busy Here"));

  h.timers->advance(1ms);
  EXPECT_EQ(transaction->state(), State::Terminated);
}

// ---- Non-INVITE client transaction, RFC 3261 17.1.2 ----

TEST(NonInviteClientTransactionTest, SendsTheRequestAndEntersTrying) {
  Harness h;
  auto transaction = make_nict(h, false);

  transaction->start(request("REGISTER"));

  EXPECT_EQ(transaction->state(), State::Trying);
  EXPECT_EQ(h.sent_method("REGISTER"), 1);
}

// Timer E doubles but is capped at T2, which is where it differs from timer A.
TEST(NonInviteClientTransactionTest, RetransmitsWithBackoffCappedAtT2) {
  Harness h;
  auto transaction = make_nict(h, false);
  transaction->start(request("REGISTER"));

  h.timers->advance(500ms);  // E = T1
  EXPECT_EQ(h.sent_method("REGISTER"), 2);

  h.timers->advance(1s);  // 2*T1
  EXPECT_EQ(h.sent_method("REGISTER"), 3);

  h.timers->advance(2s);  // 4*T1
  EXPECT_EQ(h.sent_method("REGISTER"), 4);

  h.timers->advance(4s);  // capped at T2 = 4s
  EXPECT_EQ(h.sent_method("REGISTER"), 5);

  h.timers->advance(4s);  // stays at T2, unlike timer A
  EXPECT_EQ(h.sent_method("REGISTER"), 6);
}

TEST(NonInviteClientTransactionTest, ProvisionalMovesToProceedingAndKeepsRetransmittingAtT2) {
  Harness h;
  auto register_request = request("REGISTER");
  auto transaction = make_nict(h, false);
  transaction->start(register_request);

  transaction->receive(response_to(register_request, 100, "Trying"));
  EXPECT_EQ(transaction->state(), State::Proceeding);
  ASSERT_EQ(h.to_tu.size(), 1u);

  const auto before = h.sent_method("REGISTER");

  // RFC 3261 17.1.2.2: in Proceeding, E fires at T2 flat.
  h.timers->advance(4s);
  EXPECT_EQ(h.sent_method("REGISTER"), before + 1);

  h.timers->advance(4s);
  EXPECT_EQ(h.sent_method("REGISTER"), before + 2);
}

TEST(NonInviteClientTransactionTest, FinalResponseGoesToCompleted) {
  Harness h;
  auto register_request = request("REGISTER");
  auto transaction = make_nict(h, false);
  transaction->start(register_request);

  transaction->receive(response_to(register_request, 200, "OK"));

  EXPECT_EQ(transaction->state(), State::Completed);
  ASSERT_EQ(h.to_tu.size(), 1u);

  // Retransmissions of the final response are absorbed.
  transaction->receive(response_to(register_request, 200, "OK"));
  EXPECT_EQ(h.to_tu.size(), 1u);
}

TEST(NonInviteClientTransactionTest, TimerKEndsTheTransaction) {
  Harness h;
  auto register_request = request("REGISTER");
  auto transaction = make_nict(h, false);
  transaction->start(register_request);
  transaction->receive(response_to(register_request, 200, "OK"));

  h.timers->advance(4999ms);
  EXPECT_EQ(transaction->state(), State::Completed);

  h.timers->advance(1ms);  // K = T4 = 5s
  EXPECT_EQ(transaction->state(), State::Terminated);
}

TEST(NonInviteClientTransactionTest, TimesOutWithNoResponse) {
  Harness h;
  auto transaction = make_nict(h, false);
  transaction->start(request("REGISTER"));

  h.timers->advance(32s);  // F = 64 * T1
  EXPECT_EQ(transaction->state(), State::Terminated);
  EXPECT_EQ(h.timeouts, 1);
}

TEST(NonInviteClientTransactionTest, CompletedEndsAtOnceOnAReliableTransport) {
  Harness h;
  auto register_request = request("REGISTER");
  auto transaction = make_nict(h, true);
  transaction->start(register_request);
  transaction->receive(response_to(register_request, 200, "OK"));

  h.timers->advance(1ms);
  EXPECT_EQ(transaction->state(), State::Terminated);
}
