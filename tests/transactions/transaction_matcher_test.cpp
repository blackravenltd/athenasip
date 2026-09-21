//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "transactions/invite_server_transaction.h"
#include "transactions/non_invite_server_transaction.h"
#include "transactions/transaction_matcher.h"

#include "../mocks/logger_mock.h"

using namespace athenasip;
using namespace athenasip::transactions;

namespace {

std::shared_ptr<SIPMessage> request(const std::string& method, const std::string& branch = "z9hG4bK-one", const std::string& sent_by = "alice.example.com:5060",
                                    const std::string& cseq_method = "") {
  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>(method + " sip:bob@example.com SIP/2.0\r\n" + "Via: SIP/2.0/UDP " + sent_by + ";branch=" + branch +
                                                "\r\n"
                                                "From: <sip:alice@example.com>;tag=alice\r\n"
                                                "To: <sip:bob@example.com>\r\n"
                                                "Call-ID: call-1\r\n"
                                                "CSeq: 1 " +
                                                (cseq_method.empty() ? method : cseq_method) +
                                                "\r\n"
                                                "\r\n");
  return message;
}

// An RFC 2543 request: the branch is optional and, when present, carries no magic
// cookie. Everything else is what 17.2.3's fallback matches on.
std::shared_ptr<SIPMessage> legacy_request(const std::string& method, const std::string& branch = "", const std::string& cseq_method = "",
                                           const std::string& call_id = "call-1", const std::string& to_tag = "",
                                           const std::string& request_uri = "sip:bob@example.com") {
  std::string via = "Via: SIP/2.0/UDP alice.example.com:5060";
  if (!branch.empty()) via += ";branch=" + branch;

  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>(method + " " + request_uri + " SIP/2.0\r\n" + via +
                                                "\r\n"
                                                "From: <sip:alice@example.com>;tag=alice\r\n"
                                                "To: <sip:bob@example.com>" +
                                                (to_tag.empty() ? "" : ";tag=" + to_tag) +
                                                "\r\n"
                                                "Call-ID: " +
                                                call_id +
                                                "\r\n"
                                                "CSeq: 1 " +
                                                (cseq_method.empty() ? method : cseq_method) +
                                                "\r\n"
                                                "\r\n");
  return message;
}

struct Harness {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::shared_ptr<ManualTimerSource> timers = std::make_shared<ManualTimerSource>();

  Timers values() const {
    Config config(logger);
    return Timers::from_config(config);
  }

  std::shared_ptr<TransactionBase> server(const std::string& id, bool invite) {
    auto send = [](std::shared_ptr<SIPMessage>) {};
    auto to_tu = [](std::shared_ptr<SIPMessage>) {};

    if (invite) return std::make_shared<InviteServerTransaction>(logger, id, true, values(), timers, send, to_tu);
    return std::make_shared<NonInviteServerTransaction>(logger, id, true, values(), timers, send, to_tu);
  }
};

}  // namespace

// RFC 3261 17.2.3: branch alone is not the identity. Two upstream hops can pick the same
// branch, and the same branch on two methods is two transactions.
TEST(TransactionMatcherTest, KeyIsBranchSentByAndMethod) {
  const auto key = TransactionMatcher::key(request("INVITE"));

  EXPECT_EQ(key, "z9hG4bK-one|alice.example.com:5060|INVITE");

  EXPECT_NE(key, TransactionMatcher::key(request("INVITE", "z9hG4bK-two")));
  EXPECT_NE(key, TransactionMatcher::key(request("INVITE", "z9hG4bK-one", "carol.example.com:5060")));
  EXPECT_NE(key, TransactionMatcher::key(request("OPTIONS", "z9hG4bK-one")));
}

// The separators are what stop "abc" + "INVITE" and "abcI" + "NVITE" colliding.
TEST(TransactionMatcherTest, KeyComponentsCannotRunTogether) {
  EXPECT_NE(TransactionMatcher::key(request("INVITE", "z9hG4bK-abc", "host")), TransactionMatcher::key(request("NVITE", "z9hG4bK-abcI", "host")));
}

// A message with no Via or no CSeq names no transaction at all: those two fields are
// what every form of the identity is built from.
TEST(TransactionMatcherTest, KeyIsEmptyWithoutAViaOrCSeq) {
  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>(
      "OPTIONS sip:bob@example.com SIP/2.0\r\n"
      "Via: SIP/2.0/UDP alice.example.com:5060;branch=z9hG4bK-one\r\n"
      "\r\n");

  EXPECT_TRUE(TransactionMatcher::key(message).empty());
}

TEST(TransactionMatcherTest, AddFindAndRemoveRoundTrip) {
  Harness h;
  TransactionMatcher matcher;

  auto invite = request("INVITE");
  const auto key = TransactionMatcher::key(invite);
  auto transaction = h.server(key, true);

  matcher.add(key, transaction);
  EXPECT_EQ(matcher.find(key), transaction);
  EXPECT_EQ(matcher.size(), 1u);

  EXPECT_TRUE(matcher.remove(key));
  EXPECT_EQ(matcher.find(key), nullptr);
  EXPECT_FALSE(matcher.remove(key));
}

// A retransmitted request is the same transaction, and must reach it rather than start
// a second one (RFC 3261 17.2.1).
TEST(TransactionMatcherTest, ARetransmittedRequestMatchesItsTransaction) {
  Harness h;
  TransactionMatcher matcher;

  auto invite = request("INVITE");
  matcher.add(TransactionMatcher::key(invite), h.server(TransactionMatcher::key(invite), true));

  EXPECT_NE(matcher.match_request(request("INVITE")), nullptr);
}

// RFC 3261 17.2.3: the ACK for a non-2xx carries CSeq method ACK, but belongs to the
// INVITE server transaction that sent the response. Matching on the ACK's own method
// would miss it, leave the transaction retransmitting, and hand the TU an ACK it has no
// use for.
TEST(TransactionMatcherTest, AnAckMatchesTheInviteTransactionItAcknowledges) {
  Harness h;
  TransactionMatcher matcher;

  auto invite = request("INVITE");
  auto transaction = h.server(TransactionMatcher::key(invite), true);
  matcher.add(TransactionMatcher::key(invite), transaction);

  // The ACK reuses the INVITE's branch and sent-by (17.1.1.3).
  auto ack = request("ACK", "z9hG4bK-one", "alice.example.com:5060", "ACK");

  EXPECT_EQ(matcher.match_request(ack), transaction);
}

// An ACK whose branch matches nothing is the ACK for a 2xx. That one is end to end and
// belongs to the transaction user, so the matcher must not claim it.
TEST(TransactionMatcherTest, AnUnmatchedAckIsNotClaimed) {
  TransactionMatcher matcher;

  EXPECT_EQ(matcher.match_request(request("ACK", "z9hG4bK-unknown", "alice.example.com:5060", "ACK")), nullptr);
}

// RFC 3261 9.2: a CANCEL is its own transaction and separately names the INVITE it
// cancels. Both have to be findable, and they are not the same one.
TEST(TransactionMatcherTest, ACancelFindsTheInviteItCancelsAndNotItself) {
  Harness h;
  TransactionMatcher matcher;

  auto invite = request("INVITE");
  auto invite_transaction = h.server(TransactionMatcher::key(invite), true);
  matcher.add(TransactionMatcher::key(invite), invite_transaction);

  // A CANCEL carries the branch of the request it cancels (9.1).
  auto cancel = request("CANCEL", "z9hG4bK-one", "alice.example.com:5060", "CANCEL");

  EXPECT_EQ(matcher.match_cancelled(cancel), invite_transaction);
  EXPECT_EQ(matcher.match_request(cancel), nullptr);

  auto cancel_transaction = h.server(TransactionMatcher::key(cancel), false);
  matcher.add(TransactionMatcher::key(cancel), cancel_transaction);

  EXPECT_EQ(matcher.match_request(cancel), cancel_transaction);
  EXPECT_EQ(matcher.match_cancelled(cancel), invite_transaction);
}

// RFC 3261 17.1.3: a response belongs to the client transaction whose branch is in the
// topmost Via and whose CSeq method matches.
TEST(TransactionMatcherTest, AResponseMatchesTheRequestItAnswers) {
  Harness h;
  TransactionMatcher matcher;

  auto invite = request("INVITE");
  auto transaction = h.server(TransactionMatcher::key(invite), true);
  matcher.add(TransactionMatcher::key(invite), transaction);

  auto response = invite->generate_response();
  response->header->response_code = 180;
  response->header->response_message = "Ringing";

  EXPECT_EQ(matcher.match_response(response), transaction);
}

// terminate_all fires on_terminated, which is wired to remove from the table being
// walked. Iterating it directly would invalidate under its own callbacks.
TEST(TransactionMatcherTest, TerminateAllEmptiesTheTableUnderItsOwnCallbacks) {
  Harness h;
  TransactionMatcher matcher;

  for (int i = 0; i < 16; ++i) {
    const auto key = "z9hG4bK-" + std::to_string(i) + "|alice.example.com|OPTIONS";
    auto transaction = h.server(key, false);
    transaction->on_terminated([&matcher](const std::string& id) { matcher.remove(id); });
    matcher.add(key, transaction);
  }

  ASSERT_EQ(matcher.size(), 16u);
  EXPECT_NO_THROW(matcher.terminate_all());
  EXPECT_EQ(matcher.size(), 0u);
}

// RFC 3261 17.2.3: a branch with no magic cookie was not generated by a 3261 client
// transaction, so it cannot be trusted to be unique on its own. The fallback names the
// transaction by the Request-URI, From tag, Call-ID, CSeq and the whole topmost Via -
// the branch counts only as one more parameter of that Via, not as the identity.
TEST(TransactionMatcherTest, ALegacyRequestIsNamedByTheFallbackTuple) {
  const auto key = TransactionMatcher::key(legacy_request("INVITE", "1234-not-magic"));

  EXPECT_NE(key, TransactionMatcher::key(legacy_request("INVITE", "1234-not-magic", "", "call-2")));
  EXPECT_NE(key, TransactionMatcher::key(legacy_request("INVITE", "1234-not-magic", "", "call-1", "", "sip:carol@example.com")));
  EXPECT_NE(key, TransactionMatcher::key(legacy_request("OPTIONS", "1234-not-magic")));
  EXPECT_NE(key, TransactionMatcher::key(legacy_request("INVITE", "5678-also-not-magic")));

  // Nothing of the 3261 identity is in it: that key is branch and sent-by alone, and
  // reading a non-magic branch that way is what the fallback exists to avoid.
  EXPECT_NE(key, "1234-not-magic|alice.example.com:5060|INVITE");
}

// A 2543 request had no branch to carry, and this is the case that used to be answered
// 400 for want of a transaction identifier.
TEST(TransactionMatcherTest, ARequestWithNoBranchStillNamesATransaction) {
  EXPECT_FALSE(TransactionMatcher::key(legacy_request("INVITE")).empty());
}

// The whole point of the rule: a retransmission from a 2543 client reaches the
// transaction it started rather than starting another one.
TEST(TransactionMatcherTest, ALegacyRetransmissionMatchesItsTransaction) {
  Harness h;
  TransactionMatcher matcher;

  const auto key = TransactionMatcher::key(legacy_request("INVITE"));
  auto transaction = h.server(key, true);
  matcher.add(key, transaction);

  EXPECT_EQ(matcher.match_request(legacy_request("INVITE")), transaction);

  // A second call from the same client is a different transaction.
  EXPECT_EQ(matcher.match_request(legacy_request("INVITE", "", "", "call-2")), nullptr);
}

// 17.2.3: the ACK matches on the CSeq number rather than the method, and on the To tag
// the response carried, which the INVITE that created the transaction did not have.
TEST(TransactionMatcherTest, ALegacyAckMatchesTheInviteTransactionItAcknowledges) {
  Harness h;
  TransactionMatcher matcher;

  const auto key = TransactionMatcher::key(legacy_request("INVITE", "1234-not-magic"));
  auto transaction = h.server(key, true);
  matcher.add(key, transaction);

  auto ack = legacy_request("ACK", "1234-not-magic", "ACK", "call-1", "bob-tag");

  EXPECT_EQ(matcher.match_request(ack), transaction);
}

// RFC 3261 9.2: the CANCEL is matched as though its method were the one it cancels. Its
// Request-URI, Call-ID, From, CSeq number and topmost Via are identical to the INVITE's
// (9.1), which is what makes that possible without a branch to go on.
TEST(TransactionMatcherTest, ALegacyCancelFindsTheInviteItCancelsAndNotItself) {
  Harness h;
  TransactionMatcher matcher;

  const auto key = TransactionMatcher::key(legacy_request("INVITE", "1234-not-magic"));
  auto invite_transaction = h.server(key, true);
  matcher.add(key, invite_transaction);

  auto cancel = legacy_request("CANCEL", "1234-not-magic", "CANCEL");

  EXPECT_EQ(matcher.match_cancelled(cancel), invite_transaction);
  EXPECT_EQ(matcher.match_request(cancel), nullptr);

  const auto cancel_key = TransactionMatcher::key(cancel);
  auto cancel_transaction = h.server(cancel_key, false);
  matcher.add(cancel_key, cancel_transaction);

  EXPECT_EQ(matcher.match_request(cancel), cancel_transaction);
  EXPECT_EQ(matcher.match_cancelled(cancel), invite_transaction);
}

// 17.2.3 ends by saying the server cannot match a response this way, because the rules
// include the Request-URI. A response with no magic cookie belongs to no client
// transaction here.
TEST(TransactionMatcherTest, ALegacyResponseNamesNoTransaction) {
  auto response = legacy_request("INVITE", "1234-not-magic")->generate_response();
  response->header->response_code = 200;
  response->header->response_message = "OK";

  EXPECT_TRUE(TransactionMatcher::key(response).empty());
}
