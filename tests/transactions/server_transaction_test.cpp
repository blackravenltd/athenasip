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
#include "transactions/invite_server_transaction.h"
#include "transactions/non_invite_server_transaction.h"

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
                                                "CSeq: 1 " +
                                                method +
                                                "\r\n"
                                                "\r\n");
  return message;
}

std::shared_ptr<SIPMessage> response(const std::shared_ptr<SIPMessage>& to, int code, const std::string& reason) {
  auto message = to->generate_response();
  message->header->response_code = code;
  message->header->response_message = reason;
  return message;
}

// Captures what went to the transport and what reached the transaction user.
struct Harness {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<ManualTimerSource> timers = std::make_shared<ManualTimerSource>();

  std::vector<std::shared_ptr<SIPMessage>> sent;
  std::vector<std::shared_ptr<SIPMessage>> to_tu;
  std::vector<std::string> terminated;

  Timers values() const {
    Config config(logger);
    return Timers::from_config(config);
  }

  int sent_count(int code) const {
    int count = 0;
    for (const auto& message : sent) {
      if (message->header->response_code == code) count++;
    }
    return count;
  }
};

std::shared_ptr<InviteServerTransaction> make_ist(Harness& h, bool reliable) {
  auto transaction = std::make_shared<InviteServerTransaction>(
      h.logger, "ist-1", reliable, h.values(), h.timers, [&h](std::shared_ptr<SIPMessage> m) { h.sent.push_back(m); },
      [&h](std::shared_ptr<SIPMessage> m) { h.to_tu.push_back(m); });

  transaction->on_terminated([&h](const std::string& id) { h.terminated.push_back(id); });
  return transaction;
}

std::shared_ptr<NonInviteServerTransaction> make_nist(Harness& h, bool reliable) {
  auto transaction = std::make_shared<NonInviteServerTransaction>(
      h.logger, "nist-1", reliable, h.values(), h.timers, [&h](std::shared_ptr<SIPMessage> m) { h.sent.push_back(m); },
      [&h](std::shared_ptr<SIPMessage> m) { h.to_tu.push_back(m); });

  transaction->on_terminated([&h](const std::string& id) { h.terminated.push_back(id); });
  return transaction;
}

}  // namespace

// ---- INVITE server transaction, RFC 3261 17.2.1 ----

TEST(InviteServerTransactionTest, StartsInProceedingAndPassesTheRequestUp) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ist(h, false);

  transaction->start(invite);

  EXPECT_EQ(transaction->state(), State::Proceeding);
  ASSERT_EQ(h.to_tu.size(), 1u);
  EXPECT_EQ(h.to_tu[0], invite);
}

// 17.2.1: if the TU has not answered within 200ms the transaction sends 100 Trying
// itself, so the far end stops retransmitting.
TEST(InviteServerTransactionTest, SendsOneHundredTryingWhenTheTuIsSlow) {
  Harness h;
  auto transaction = make_ist(h, false);
  transaction->start(request("INVITE"));

  EXPECT_TRUE(h.sent.empty());

  h.timers->advance(200ms);

  ASSERT_EQ(h.sent.size(), 1u);
  EXPECT_EQ(h.sent[0]->header->response_code, 100);
}

TEST(InviteServerTransactionTest, DoesNotSendOneHundredWhenTheTuAlreadyResponded) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ist(h, false);
  transaction->start(invite);

  transaction->send(response(invite, 180, "Ringing"));
  h.timers->advance(1s);

  EXPECT_EQ(h.sent_count(100), 0);
  EXPECT_EQ(h.sent_count(180), 1);
}

// A retransmitted INVITE must be answered by the transaction, and must not reach the
// TU a second time.
TEST(InviteServerTransactionTest, AbsorbsRetransmittedInvites) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ist(h, false);
  transaction->start(invite);
  transaction->send(response(invite, 180, "Ringing"));

  transaction->receive(invite);
  transaction->receive(invite);

  EXPECT_EQ(h.to_tu.size(), 1u) << "the TU saw the INVITE more than once";
  EXPECT_EQ(h.sent_count(180), 3) << "each retransmission should get the 180 back";
}

// RFC 6026 section 7.1, which replaces RFC 3261 17.2.1 here: a 2xx moves the transaction
// to Accepted rather than ending it.
TEST(InviteServerTransactionTest, TwoHundredGoesToAccepted) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ist(h, false);
  transaction->start(invite);

  transaction->send(response(invite, 200, "OK"));

  EXPECT_EQ(transaction->state(), State::Accepted);
  EXPECT_EQ(h.sent_count(200), 1);
}

// In Accepted the TU's 2xx retransmissions go out through the transaction, which is how a
// proxy's copy of a callee's retransmitted 2xx reaches the caller on the connection the
// INVITE came in on; and a retransmitted INVITE is absorbed, not passed up again.
TEST(InviteServerTransactionTest, AcceptedSendsTheTusTwoHundredsAndAbsorbsTheInvite) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ist(h, true);
  transaction->start(invite);
  const auto passed_up = h.to_tu.size();

  transaction->send(response(invite, 200, "OK"));
  transaction->send(response(invite, 200, "OK"));
  EXPECT_EQ(h.sent_count(200), 2);

  transaction->receive(invite);
  EXPECT_EQ(h.to_tu.size(), passed_up);
  EXPECT_EQ(h.sent_count(200), 2) << "the transaction does not retransmit a 2xx itself";
}

// Timer L, 64*T1, ends Accepted.
TEST(InviteServerTransactionTest, TimerLEndsAccepted) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ist(h, true);
  transaction->start(invite);
  transaction->send(response(invite, 200, "OK"));

  h.timers->advance(h.values().l - 1ms);
  EXPECT_EQ(transaction->state(), State::Accepted);

  h.timers->advance(1ms);
  EXPECT_EQ(transaction->state(), State::Terminated);
  ASSERT_EQ(h.terminated.size(), 1u);
}

TEST(InviteServerTransactionTest, FinalErrorGoesToCompleted) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ist(h, false);
  transaction->start(invite);

  transaction->send(response(invite, 486, "Busy Here"));

  EXPECT_EQ(transaction->state(), State::Completed);
  EXPECT_EQ(h.sent_count(486), 1);
}

// Timer G retransmits the final response, doubling up to T2.
TEST(InviteServerTransactionTest, RetransmitsTheFinalResponseWithBackoff) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ist(h, false);
  transaction->start(invite);
  transaction->send(response(invite, 486, "Busy Here"));

  EXPECT_EQ(h.sent_count(486), 1);

  h.timers->advance(500ms);  // G = T1
  EXPECT_EQ(h.sent_count(486), 2);

  h.timers->advance(1s);  // 2*T1
  EXPECT_EQ(h.sent_count(486), 3);

  h.timers->advance(2s);  // 4*T1
  EXPECT_EQ(h.sent_count(486), 4);

  h.timers->advance(4s);  // 8*T1 capped at T2 = 4s
  EXPECT_EQ(h.sent_count(486), 5);

  h.timers->advance(4s);  // stays at T2
  EXPECT_EQ(h.sent_count(486), 6);
}

// On a reliable transport there is nothing to retransmit.
TEST(InviteServerTransactionTest, DoesNotRetransmitOnAReliableTransport) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ist(h, true);
  transaction->start(invite);
  transaction->send(response(invite, 486, "Busy Here"));

  h.timers->advance(10s);

  EXPECT_EQ(h.sent_count(486), 1);
}

// The ACK for a non-2xx is absorbed by the transaction and never reaches the TU.
TEST(InviteServerTransactionTest, AckForANonTwoHundredIsAbsorbed) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ist(h, false);
  transaction->start(invite);
  transaction->send(response(invite, 486, "Busy Here"));

  const auto tu_before = h.to_tu.size();
  transaction->receive(request("ACK"));

  EXPECT_EQ(transaction->state(), State::Confirmed);
  EXPECT_EQ(h.to_tu.size(), tu_before) << "the ACK for a non-2xx must not reach the TU";
}

TEST(InviteServerTransactionTest, ConfirmedAbsorbsAckRetransmissionsThenTerminates) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ist(h, false);
  transaction->start(invite);
  transaction->send(response(invite, 486, "Busy Here"));
  transaction->receive(request("ACK"));

  const auto sent_before = h.sent.size();
  transaction->receive(request("ACK"));
  transaction->receive(request("ACK"));
  EXPECT_EQ(h.sent.size(), sent_before) << "ACK retransmissions must not be answered";

  h.timers->advance(5s);  // timer I = T4
  EXPECT_EQ(transaction->state(), State::Terminated);
}

TEST(InviteServerTransactionTest, ConfirmedEndsAtOnceOnAReliableTransport) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ist(h, true);
  transaction->start(invite);
  transaction->send(response(invite, 486, "Busy Here"));
  transaction->receive(request("ACK"));

  h.timers->advance(1ms);
  EXPECT_EQ(transaction->state(), State::Terminated);
}

// Timer H: no ACK ever arrives, so the transaction gives up after 64*T1.
TEST(InviteServerTransactionTest, GivesUpWhenNoAckArrives) {
  Harness h;
  auto invite = request("INVITE");
  auto transaction = make_ist(h, false);
  transaction->start(invite);
  transaction->send(response(invite, 486, "Busy Here"));

  h.timers->advance(31s);
  EXPECT_EQ(transaction->state(), State::Completed);

  h.timers->advance(1s);  // H = 64 * T1 = 32s
  EXPECT_EQ(transaction->state(), State::Terminated);
  ASSERT_EQ(h.terminated.size(), 1u);
}

// ---- Non-INVITE server transaction, RFC 3261 17.2.2 ----

TEST(NonInviteServerTransactionTest, StartsInTryingAndPassesTheRequestUp) {
  Harness h;
  auto register_request = request("REGISTER");
  auto transaction = make_nist(h, false);

  transaction->start(register_request);

  EXPECT_EQ(transaction->state(), State::Trying);
  ASSERT_EQ(h.to_tu.size(), 1u);
}

// 17.2.2: in Trying there is nothing to send, so a retransmission is simply dropped.
// The TU must not see the request twice.
TEST(NonInviteServerTransactionTest, DropsRetransmissionsWhileTrying) {
  Harness h;
  auto register_request = request("REGISTER");
  auto transaction = make_nist(h, false);
  transaction->start(register_request);

  transaction->receive(register_request);
  transaction->receive(register_request);

  EXPECT_EQ(h.to_tu.size(), 1u);
  EXPECT_TRUE(h.sent.empty());
}

TEST(NonInviteServerTransactionTest, ProvisionalMovesToProceedingAndIsRepeated) {
  Harness h;
  auto options = request("OPTIONS");
  auto transaction = make_nist(h, false);
  transaction->start(options);

  transaction->send(response(options, 100, "Trying"));
  EXPECT_EQ(transaction->state(), State::Proceeding);

  transaction->receive(options);
  EXPECT_EQ(h.sent_count(100), 2);
  EXPECT_EQ(h.to_tu.size(), 1u);
}

TEST(NonInviteServerTransactionTest, FinalResponseGoesToCompletedAndIsRepeated) {
  Harness h;
  auto register_request = request("REGISTER");
  auto transaction = make_nist(h, false);
  transaction->start(register_request);

  transaction->send(response(register_request, 200, "OK"));
  EXPECT_EQ(transaction->state(), State::Completed);

  transaction->receive(register_request);
  transaction->receive(register_request);
  EXPECT_EQ(h.sent_count(200), 3);
  EXPECT_EQ(h.to_tu.size(), 1u);
}

// Timer J holds the transaction open only to answer retransmissions.
TEST(NonInviteServerTransactionTest, TimerJEndsTheTransaction) {
  Harness h;
  auto register_request = request("REGISTER");
  auto transaction = make_nist(h, false);
  transaction->start(register_request);
  transaction->send(response(register_request, 200, "OK"));

  h.timers->advance(31s);
  EXPECT_EQ(transaction->state(), State::Completed);

  h.timers->advance(1s);  // J = 64 * T1 = 32s
  EXPECT_EQ(transaction->state(), State::Terminated);
}

TEST(NonInviteServerTransactionTest, EndsAtOnceOnAReliableTransport) {
  Harness h;
  auto register_request = request("REGISTER");
  auto transaction = make_nist(h, true);
  transaction->start(register_request);
  transaction->send(response(register_request, 200, "OK"));

  h.timers->advance(1ms);
  EXPECT_EQ(transaction->state(), State::Terminated);
}

TEST(NonInviteServerTransactionTest, IgnoresAResponseAfterTheFinalOne) {
  Harness h;
  auto register_request = request("REGISTER");
  auto transaction = make_nist(h, false);
  transaction->start(register_request);

  transaction->send(response(register_request, 200, "OK"));
  transaction->send(response(register_request, 500, "Server Error"));

  EXPECT_EQ(h.sent_count(500), 0);
  EXPECT_EQ(transaction->last_response()->header->response_code, 200);
}
