//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// RFC 3261 10 from the client's side: this node registering to a carrier.
#include "trunk_registrar.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "digest.h"
#include "headers/cseq_header.h"
#include "helpers/core_fixture_helper.h"
#include "types/authorization.h"
#include "util.h"

using namespace athenasip;

namespace {

std::string first(const std::shared_ptr<SIPMessage>& message, const std::string& field) {
  return message->header->contains(field) ? message->header->headers_map[field][0]->to_string() : "";
}

std::uint64_t cseq_of(const std::shared_ptr<SIPMessage>& message) {
  auto cseq = message->header->headers_map["CSeq"][0]->as<headers::CSeqHeader>();
  return cseq ? cseq->sequence : 0;
}

struct RegistrarFixture : CoreFixture {
  std::shared_ptr<MockConnection> carrier_connection;
  std::shared_ptr<Channel> carrier;

  RegistrarFixture() {
    carrier = make_channel("192.0.2.30", &carrier_connection);

    auto acme = std::make_shared<types::Trunk>();
    acme->name = "acme";
    acme->uri = "sip:192.0.2.30:5060";
    acme->username = "4420";
    acme->password = "s3cret";
    acme->register_enabled = true;
    acme->register_expires = 300;
    acme->contact_user = "4420";
    EXPECT_TRUE(store->trunk_create(acme));
  }

  ~RegistrarFixture() {
    on_strand([this]() { core->trunk_registrar()->stop(); });
  }

  void start() {
    on_strand([this]() { core->trunk_registrar()->start(); });
    settle();
  }

  std::vector<std::shared_ptr<SIPMessage>> registers() {
    std::vector<std::shared_ptr<SIPMessage>> found;
    for (const auto& message : written(carrier_connection)) {
      if (message->header->type == SIPHeader::Type::Request && message->header->request_method == "REGISTER") found.push_back(message);
    }
    return found;
  }

  void answer(const std::shared_ptr<SIPMessage>& request, int code, const std::string& reason, const std::string& extra = "") {
    std::string raw = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n";
    for (const auto& via : request->header->headers_map["Via"]) raw += "Via: " + via->to_string() + "\r\n";
    raw += "From: " + first(request, "From") + "\r\n";
    raw += "To: " + first(request, "To") + ";tag=registrar\r\n";
    raw += "Call-ID: " + first(request, "Call-ID") + "\r\n";
    raw += "CSeq: " + first(request, "CSeq") + "\r\n";
    raw += extra;
    raw += "\r\n";
    receive(carrier, raw);
  }

  TrunkRegistrar::Status status() {
    return on_strand([this]() { return core->trunk_registrar()->statuses()["acme"]; });
  }

  void advance(std::chrono::seconds by) {
    timers->advance(by);
    settle();
  }
};

const char* kChallenge = "WWW-Authenticate: Digest realm=\"acme.example\", nonce=\"r3g\", algorithm=MD5\r\n";

}  // namespace

// RFC 3261 10.2: a REGISTER for the trunk's address of record, at the carrier's domain, asking for the trunk's time.
TEST(TrunkRegistrarTest, ATrunkThatAsksIsRegisteredTo) {
  RegistrarFixture f;
  f.start();

  const auto sent = f.registers();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0]->header->request_uri->to_string(), "sip:192.0.2.30:5060");
  EXPECT_NE(first(sent[0], "To").find("sip:4420@192.0.2.30"), std::string::npos) << first(sent[0], "To");
  EXPECT_NE(first(sent[0], "Contact").find("sip:4420@"), std::string::npos) << first(sent[0], "Contact");
  EXPECT_EQ(first(sent[0], "Expires"), "300");
  EXPECT_EQ(f.status().state, "registering");
}

// The carrier's challenge is answered with the trunk's credentials, in the same Call-ID with the next CSeq.
TEST(TrunkRegistrarTest, TheCarriersChallengeIsAnswered) {
  RegistrarFixture f;
  f.start();

  f.answer(f.registers()[0], 401, "Unauthorized", kChallenge);

  const auto sent = f.registers();
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(first(sent[1], "Call-ID"), first(sent[0], "Call-ID"));
  EXPECT_EQ(cseq_of(sent[1]), cseq_of(sent[0]) + 1);

  types::Authorization credentials(first(sent[1], "Authorization"));
  types::Subscriber as_the_carrier_knows_it;
  as_the_carrier_knows_it.ha1 = Util::md5("4420:acme.example:s3cret");
  EXPECT_EQ(digest::verify(as_the_carrier_knows_it, credentials, "REGISTER"), "");

  f.answer(sent[1], 200, "OK", "Contact: " + first(sent[1], "Contact") + ";expires=120\r\n");
  const auto status = f.status();
  EXPECT_EQ(status.state, "registered");
  EXPECT_GT(status.expires_at, std::time(nullptr) + 100);
}

// Refreshed before the carrier's binding lapses: a minute early, or at three quarters of a short grant.
TEST(TrunkRegistrarTest, ARegistrationIsRefreshedBeforeItLapses) {
  RegistrarFixture f;
  f.start();
  f.answer(f.registers()[0], 200, "OK", "Expires: 300\r\n");
  ASSERT_EQ(f.status().state, "registered");

  f.advance(std::chrono::seconds(239));
  EXPECT_EQ(f.registers().size(), 1u);
  f.advance(std::chrono::seconds(1));
  ASSERT_EQ(f.registers().size(), 2u) << "300 - 60";

  f.answer(f.registers()[1], 200, "OK", "Expires: 100\r\n");
  f.advance(std::chrono::seconds(74));
  EXPECT_EQ(f.registers().size(), 2u);
  f.advance(std::chrono::seconds(1));
  EXPECT_EQ(f.registers().size(), 3u) << "three quarters of 100";
}

// A carrier that refuses is tried again later, not at once, and the failure says why.
TEST(TrunkRegistrarTest, ARefusalIsRetriedWithBackoff) {
  RegistrarFixture f;
  f.start();
  f.answer(f.registers()[0], 403, "Forbidden");

  EXPECT_EQ(f.status().state, "failed");
  EXPECT_EQ(f.status().detail, "403 Forbidden");

  f.advance(std::chrono::seconds(29));
  EXPECT_EQ(f.registers().size(), 1u);
  f.advance(std::chrono::seconds(1));
  EXPECT_EQ(f.registers().size(), 2u);
}

// Credentials the carrier refuses are not sent again in a loop.
TEST(TrunkRegistrarTest, RefusedCredentialsAreNotSentAgainAndAgain) {
  RegistrarFixture f;
  f.start();
  f.answer(f.registers()[0], 401, "Unauthorized", kChallenge);
  f.answer(f.registers()[1], 401, "Unauthorized", kChallenge);

  EXPECT_EQ(f.registers().size(), 2u);
  EXPECT_EQ(f.status().state, "failed");
}

// RFC 3261 10.3 step 7: asked for too little, the next REGISTER asks for the carrier's minimum.
TEST(TrunkRegistrarTest, TooBriefAsksAgainForTheMinimum) {
  RegistrarFixture f;
  f.start();
  f.answer(f.registers()[0], 423, "Interval Too Brief", "Min-Expires: 600\r\n");

  ASSERT_EQ(f.registers().size(), 2u);
  EXPECT_EQ(first(f.registers()[1], "Expires"), "600");
}

// One node of a cluster registers to a trunk: a lease another node holds keeps this one out.
TEST(TrunkRegistrarTest, OnlyTheNodeHoldingTheLeaseRegisters) {
  RegistrarFixture f;
  ASSERT_TRUE(f.store->lease("trunk-register:acme", "another-node", 90));

  f.start();
  EXPECT_TRUE(f.registers().empty());
}

// A trunk that no longer asks to be registered has its binding removed (RFC 3261 10.2.2), then is left alone.
TEST(TrunkRegistrarTest, ATrunkThatStopsAskingIsUnregistered) {
  RegistrarFixture f;
  f.start();
  f.answer(f.registers()[0], 200, "OK", "Expires: 3600\r\n");

  auto acme = f.store->trunk_get("acme");
  acme->register_enabled = false;
  ASSERT_TRUE(f.store->trunk_update(acme));

  f.advance(TrunkRegistrar::kScan);
  const auto sent = f.registers();
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(first(sent[1], "Expires"), "0");
  EXPECT_EQ(first(sent[1], "Call-ID"), first(sent[0], "Call-ID"));

  f.answer(sent[1], 200, "OK");
  EXPECT_TRUE(f.on_strand([&f]() { return f.core->trunk_registrar()->statuses().empty(); }));

  f.advance(TrunkRegistrar::kScan);
  EXPECT_EQ(f.registers().size(), 2u) << "nothing more is sent";
}

// A node that loses the lease stops without unregistering: the binding is now the other node's to keep.
TEST(TrunkRegistrarTest, ANodeThatLosesTheLeaseSendsNothing) {
  RegistrarFixture f;
  f.start();
  f.answer(f.registers()[0], 200, "OK", "Expires: 3600\r\n");

  // Another node takes the lease once this one's has lapsed.
  f.on_strand([&f]() { f.core->trunk_registrar()->stop(); });
  ASSERT_TRUE(f.store->lease("trunk-register:acme", "test-node", 1));
  std::this_thread::sleep_for(std::chrono::milliseconds(2100));
  ASSERT_TRUE(f.store->lease("trunk-register:acme", "another-node", 90));

  f.on_strand([&f]() { f.core->trunk_registrar()->scan(); });
  f.settle();
  EXPECT_EQ(f.registers().size(), 1u);
  EXPECT_TRUE(f.on_strand([&f]() { return f.core->trunk_registrar()->statuses().empty(); }));
}
