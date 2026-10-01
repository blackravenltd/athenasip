//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "qualifier.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

#include "helpers/core_fixture_helper.h"
#include "util.h"

using namespace athenasip;

namespace {

const std::string kHa1 = Util::md5("alice:example.com:secret");

// What a WebRTC client answers an OPTIONS with when it says what it takes (RFC 3261 11.2).
const std::string kWebRtcCapabilities =
    "v=0\r\n"
    "o=- 1 1 IN IP4 0.0.0.0\r\n"
    "s=-\r\n"
    "t=0 0\r\n"
    "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
    "c=IN IP4 0.0.0.0\r\n"
    "a=rtpmap:111 opus/48000/2\r\n"
    "a=ice-ufrag:4ZcD\r\n"
    "a=ice-pwd:2/rEckDvYgxFs9WU3wMcYY\r\n"
    "a=fingerprint:sha-256 AA:BB:CC\r\n";

struct Fixture : CoreFixture {
  std::shared_ptr<MockConnection> connection;
  std::shared_ptr<Channel> channel;

  explicit Fixture(std::optional<std::uint32_t> realm_interval = std::nullopt, std::uint32_t server_interval = 0) {
    config->behaviour_qualify_interval = server_interval;

    auto realm = seed_realm("example.com");
    realm->behaviour.qualify_interval = realm_interval;
    store->realm_update(realm);

    seed_account(7, "sip:alice@example.com", kHa1);
    channel = make_channel("192.0.2.10", &connection);
  }

  std::string register_request(const std::string& expires = "3600", const std::string& branch = "z9hG4bK-reg") {
    const auto nonce = mint_nonce(store->realm_get_by_name("example.com"));
    const auto response = Util::md5(kHa1 + ":" + nonce + ":" + Util::md5("REGISTER:sip:example.com"));

    std::string raw = "REGISTER sip:example.com SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=" + branch + "\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:alice@example.com>\r\n";
    raw += "Call-ID: call-registrar\r\n";
    raw += "CSeq: 1 REGISTER\r\n";
    raw += "Contact: <sip:alice@192.0.2.10:5060>\r\n";
    raw += "Expires: " + expires + "\r\n";
    raw +=
        "Authorization: Digest username=\"alice\", realm=\"example.com\", nonce=\"" + nonce + "\", uri=\"sip:example.com\", response=\"" + response + "\"\r\n";
    raw += "\r\n";
    return raw;
  }

  void tick(std::chrono::milliseconds by) {
    timers->advance(by);
    settle();
  }

  std::vector<std::shared_ptr<SIPMessage>> probes() {
    std::vector<std::shared_ptr<SIPMessage>> found;
    for (const auto& message : written(connection)) {
      if (message->header->type == SIPHeader::Type::Request && message->header->request_method == "OPTIONS") found.push_back(message);
    }
    return found;
  }

  // The client's answer to the latest probe, as a UAS builds one (RFC 3261 8.2.6).
  std::string answer(int code, const std::string& sdp = "") {
    const auto sent = probes();
    if (sent.empty()) return "";
    const auto& header = sent.back()->header;

    std::string raw = "SIP/2.0 " + std::to_string(code) + " " + (code == 200 ? "OK" : "Method Not Allowed") + "\r\n";
    raw += "Via: " + header->headers_map["Via"][0]->to_string() + "\r\n";
    raw += "From: " + header->headers_map["From"][0]->to_string() + "\r\n";
    raw += "To: " + header->headers_map["To"][0]->to_string() + ";tag=client\r\n";
    raw += "Call-ID: " + header->headers_map["Call-ID"][0]->to_string() + "\r\n";
    raw += "CSeq: " + header->headers_map["CSeq"][0]->to_string() + "\r\n";
    if (!sdp.empty()) raw += "Content-Type: application/sdp\r\nContent-Length: " + std::to_string(sdp.size()) + "\r\n";
    raw += "\r\n";
    return raw + sdp;
  }

  std::vector<Qualifier::Probe> watched() {
    return on_strand([this]() { return core->qualifier()->list(); });
  }

  std::optional<media::Profile> said() {
    return on_strand([this]() { return core->qualifier()->said(channel->flow_id()); });
  }
};

}  // namespace

// RFC 3261 does not ask a registrar to probe anybody, so nothing is sent unless the
// operator has said how often.
TEST(QualifierTest, NothingIsProbedByDefault) {
  Fixture f;

  f.receive(f.channel, f.register_request());
  ASSERT_NE(f.response_with(f.connection, 200), nullptr);

  f.tick(std::chrono::seconds(600));
  EXPECT_TRUE(f.probes().empty());
}

// Asterisk's qualify: an OPTIONS down the flow the client registered on, straight after
// it registers and then on the realm's interval. RFC 3261 11.1: addressed to the contact,
// To the address of record, asking for a session description in Accept.
TEST(QualifierTest, ARegisteredClientIsProbedOnItsRealmsInterval) {
  Fixture f(30);

  f.receive(f.channel, f.register_request());
  f.tick(std::chrono::milliseconds(1));

  auto sent = f.probes();
  ASSERT_EQ(sent.size(), 1u);

  const auto& header = sent[0]->header;
  EXPECT_EQ(header->request_uri->to_string(), "sip:alice@192.0.2.10:5060");
  EXPECT_NE(header->headers_map["To"][0]->to_string().find("sip:alice@example.com"), std::string::npos);
  EXPECT_EQ(header->headers_map["To"][0]->to_string().find("tag="), std::string::npos);
  EXPECT_NE(header->headers_map["From"][0]->to_string().find("tag="), std::string::npos);
  EXPECT_NE(header->headers_map["Via"][0]->to_string().find("branch=z9hG4bK"), std::string::npos);
  EXPECT_NE(header->headers_map["CSeq"][0]->to_string().find("OPTIONS"), std::string::npos);
  EXPECT_EQ(header->headers_map["Max-Forwards"][0]->to_string(), "70");
  EXPECT_NE(Util::to_lower(header->headers_map["Accept"][0]->to_string()).find("application/sdp"), std::string::npos);

  f.receive(f.channel, f.answer(200));
  f.tick(std::chrono::seconds(29));
  EXPECT_EQ(f.probes().size(), 1u) << "not before the interval";

  f.tick(std::chrono::seconds(1));
  sent = f.probes();
  ASSERT_EQ(sent.size(), 2u);

  // One conversation: the same Call-ID, the CSeq counting up.
  EXPECT_EQ(sent[0]->header->headers_map["Call-ID"][0]->to_string(), sent[1]->header->headers_map["Call-ID"][0]->to_string());
  EXPECT_NE(sent[0]->header->headers_map["CSeq"][0]->to_string(), sent[1]->header->headers_map["CSeq"][0]->to_string());
}

// The server's default applies to a realm that says nothing, and a realm can turn it off.
TEST(QualifierTest, TheServerDefaultAppliesUnlessTheRealmTurnsItOff) {
  Fixture inherits(std::nullopt, 60);
  inherits.receive(inherits.channel, inherits.register_request());
  inherits.tick(std::chrono::milliseconds(1));
  EXPECT_EQ(inherits.probes().size(), 1u);

  Fixture off(0, 60);
  off.receive(off.channel, off.register_request());
  off.tick(std::chrono::seconds(120));
  EXPECT_TRUE(off.probes().empty());
}

// RFC 3261 11.2: the 200 may carry the description the client would have offered, and
// that is the client saying what media it takes.
TEST(QualifierTest, TheDescriptionInTheAnswerIsWhatTheClientSaid) {
  Fixture f(30);

  f.receive(f.channel, f.register_request());
  f.tick(std::chrono::milliseconds(1));
  EXPECT_FALSE(f.said().has_value());

  f.receive(f.channel, f.answer(200, kWebRtcCapabilities));
  EXPECT_EQ(f.said(), media::Profile::WebRtc);

  // A later answer with no description says nothing about media, which does not undo what
  // was said.
  f.tick(std::chrono::seconds(30));
  f.receive(f.channel, f.answer(200));
  EXPECT_EQ(f.said(), media::Profile::WebRtc);
}

// Any final answer means something is listening: a client that does not implement OPTIONS
// answers 405 (RFC 3261 8.2.1), which is as alive as a 200.
TEST(QualifierTest, AnyFinalAnswerCountsAsThere) {
  Fixture f(30);

  f.receive(f.channel, f.register_request());
  f.tick(std::chrono::milliseconds(1));
  f.receive(f.channel, f.answer(405));

  const auto probes = f.watched();
  ASSERT_EQ(probes.size(), 1u);
  EXPECT_EQ(probes[0].unanswered, 0u);
  EXPECT_NE(probes[0].answered_at, 0);
}

// Silence is counted and the probing goes on: a client that is briefly unreachable is not
// dropped by this, and the count is what an operator reads.
TEST(QualifierTest, SilenceIsCountedAndTheProbingGoesOn) {
  Fixture f(30);

  f.receive(f.channel, f.register_request());
  f.tick(std::chrono::milliseconds(1));

  // Timer F, 64*T1 (RFC 3261 17.1.2.2).
  f.tick(std::chrono::seconds(33));

  const auto probes = f.watched();
  ASSERT_EQ(probes.size(), 1u);
  EXPECT_EQ(probes[0].unanswered, 1u);

  f.tick(std::chrono::seconds(30));
  EXPECT_GE(f.probes().size(), 2u) << "probed again after the silence";
}

// A binding the client removes stops being probed (RFC 3261 10.2.2).
TEST(QualifierTest, ARemovedBindingIsNoLongerProbed) {
  Fixture f(30);

  f.receive(f.channel, f.register_request());
  f.tick(std::chrono::milliseconds(1));
  f.receive(f.channel, f.answer(200));
  ASSERT_EQ(f.probes().size(), 1u);

  f.receive(f.channel, f.register_request("0", "z9hG4bK-unreg"));
  f.tick(std::chrono::seconds(120));

  EXPECT_EQ(f.probes().size(), 1u);
  EXPECT_TRUE(f.watched().empty());
}
