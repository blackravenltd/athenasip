//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// A call out through a trunk whose carrier challenges it. RFC 3261 22.2: the node answers with the trunk's
// credentials in a new request with the next CSeq; neither end may see a CSeq it did not send, for the rest of the
// dialog.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "digest.h"
#include "headers/cseq_header.h"
#include "helpers/proxy_fixture_helper.h"
#include "policy/policy.h"
#include "types/trunk.h"
#include "util.h"

using namespace athenasip;

namespace {

// Every call goes out by the trunk acme; anything from 192.0.2.30 is acme.
class TrunkPolicy final : public policy::Policy {
 public:
  std::string name() const override { return "trunk-test"; }
  std::string version() const override { return "0.0.0"; }

  // Whether a request from the carrier's address is trusted as the trunk's, as athenasip.trunks does.
  bool trust_carrier = false;

  void authorize(plugins::Executor on, std::shared_ptr<policy::RequestView> request, plugins::Handler<policy::AuthDecision> handler) override {
    const auto channel = request->message->channel.lock();
    const bool from_carrier = channel && channel->_connection->remote_endpoint().address().to_string() == "192.0.2.30";
    _complete(
        on, handler,
        plugins::Result<policy::AuthDecision>::success(trust_carrier && from_carrier ? policy::AuthDecision::trusted("acme") : policy::AuthDecision::accept()));
  }

  // What the script would have done with request:set_from and request:set_header.
  std::vector<policy::HeaderEdit> edits;

  void route(plugins::Executor on, std::shared_ptr<policy::RequestView> request, plugins::Handler<policy::RouteDecision> handler) override {
    request->edits = edits;
    auto uri = std::make_shared<types::SIPUri>("sip:" + request->message->header->request_uri->user + "@192.0.2.30:5060");
    _complete(on, handler, plugins::Result<policy::RouteDecision>::success(policy::RouteDecision::forward({policy::Target::to(uri, nullptr, "acme")})));
  }

  void register_(plugins::Executor on, std::shared_ptr<policy::RequestView> request, plugins::Handler<policy::RegisterDecision> handler) override {
    (void)request;
    _complete(on, handler, plugins::Result<policy::RegisterDecision>::success(policy::RegisterDecision::reject(403, "Forbidden")));
  }
};

std::uint64_t cseq_of(const std::shared_ptr<SIPMessage>& message) {
  auto cseq = message->header->headers_map["CSeq"][0]->as<headers::CSeqHeader>();
  return cseq ? cseq->sequence : 0;
}

std::string first(const std::shared_ptr<SIPMessage>& message, const std::string& field) {
  return message->header->contains(field) ? message->header->headers_map[field][0]->to_string() : "";
}

struct TrunkFixture : ProxyFixture {
  std::shared_ptr<MockConnection> carrier_connection;
  std::shared_ptr<Channel> carrier;
  std::shared_ptr<TrunkPolicy> policy = std::make_shared<TrunkPolicy>();

  TrunkFixture() {
    carrier = make_channel("192.0.2.30", &carrier_connection);
    on_strand([this]() { core->policy_register(policy); });

    auto acme = std::make_shared<types::Trunk>();
    acme->name = "acme";
    acme->uri = "sip:192.0.2.30:5060";
    acme->username = "4420";
    acme->password = "s3cret";
    EXPECT_TRUE(store->trunk_create(acme));
  }

  // The carrier's answer to a request it was sent: the Via chain, the dialog fields and the CSeq as it received them.
  static std::string reply(const std::shared_ptr<SIPMessage>& request, int code, const std::string& reason, const std::string& extra = "") {
    std::string raw = "SIP/2.0 " + std::to_string(code) + " " + reason + "\r\n";
    for (const auto& via : request->header->headers_map["Via"]) raw += "Via: " + via->to_string() + "\r\n";
    for (const auto& route : request->header->headers_map["Record-Route"]) raw += "Record-Route: " + route->to_string() + "\r\n";
    raw += "From: " + first(request, "From") + "\r\n";
    auto to = first(request, "To");
    if (code != 100 && to.find("tag=") == std::string::npos) to += ";tag=carrier";
    raw += "To: " + to + "\r\n";
    raw += "Call-ID: " + first(request, "Call-ID") + "\r\n";
    raw += "CSeq: " + first(request, "CSeq") + "\r\n";
    raw += extra;
    raw += "\r\n";
    return raw;
  }

  std::vector<std::shared_ptr<SIPMessage>> to_carrier(const std::string& method) { return requests_with(carrier_connection, method); }

  std::string invite_to_number() {
    auto raw = invite("z9hG4bK-trunk", "sip:+442071234567@example.com");
    return raw;
  }
};

const char* kChallenge = "Proxy-Authenticate: Digest realm=\"acme.example\", nonce=\"c4rr13r\", algorithm=MD5, qop=\"auth\"\r\n";

}  // namespace

// The carrier's 407 is answered here, with the trunk's credentials, in a new transaction with the next CSeq; the
// caller never sees the challenge.
TEST(ProxyTrunkTest, ACarriersChallengeIsAnsweredWithTheTrunksCredentials) {
  TrunkFixture f;

  f.receive(f.caller, f.invite_to_number());
  auto sent = f.to_carrier("INVITE");
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(cseq_of(sent[0]), 1u);

  f.receive(f.carrier, TrunkFixture::reply(sent[0], 407, "Proxy Authentication Required", kChallenge));

  sent = f.to_carrier("INVITE");
  ASSERT_EQ(sent.size(), 2u) << "the INVITE again, with credentials";
  const auto& retried = sent[1];
  EXPECT_EQ(cseq_of(retried), 2u);
  EXPECT_NE(first(retried, "Via"), first(sent[0], "Via")) << "a new transaction, so a new branch";
  EXPECT_EQ(first(retried, "Call-ID"), first(sent[0], "Call-ID"));

  // What it answered, a carrier holding the same password accepts.
  ASSERT_TRUE(retried->header->contains("Proxy-Authorization"));
  types::Authorization credentials(first(retried, "Proxy-Authorization"));
  types::Subscriber as_the_carrier_knows_it;
  as_the_carrier_knows_it.ha1 = Util::md5("4420:acme.example:s3cret");
  EXPECT_EQ(digest::verify(as_the_carrier_knows_it, credentials, "INVITE"), "");
  EXPECT_EQ(credentials.fields["username"], "4420");

  // The 407 was ACKed, as every non-2xx is (RFC 3261 17.1.1.3), and never reached the caller.
  EXPECT_EQ(f.to_carrier("ACK").size(), 1u);
  EXPECT_EQ(ProxyFixture::response_with(f.caller_connection, 407), nullptr);
}

// The rest of the call is in the caller's CSeq space on the caller's side and the carrier's on the carrier's.
TEST(ProxyTrunkTest, TheCallGoesOnInEachEndsOwnCSeqSpace) {
  TrunkFixture f;

  f.receive(f.caller, f.invite_to_number());
  f.receive(f.carrier, TrunkFixture::reply(f.to_carrier("INVITE")[0], 407, "Proxy Authentication Required", kChallenge));
  const auto retried = f.to_carrier("INVITE").back();

  f.receive(f.carrier, TrunkFixture::reply(retried, 180, "Ringing"));
  auto ringing = ProxyFixture::response_with(f.caller_connection, 180);
  ASSERT_NE(ringing, nullptr);
  EXPECT_EQ(cseq_of(ringing), 1u);

  f.receive(f.carrier, TrunkFixture::reply(retried, 200, "OK", "Contact: <sip:+442071234567@192.0.2.30:5060>\r\n"));
  auto answered = ProxyFixture::response_with(f.caller_connection, 200);
  ASSERT_NE(answered, nullptr);
  EXPECT_EQ(cseq_of(answered), 1u) << "the caller's own INVITE is answered";

  // The caller's ACK and BYE, through the Record-Route it was given.
  std::string route;
  for (const auto& value : answered->header->headers_map["Record-Route"]) route = "Route: " + value->to_string() + "\r\n" + route;
  const auto in_dialog = [&](const std::string& method, int cseq) {
    std::string raw = method + " sip:+442071234567@192.0.2.30:5060 SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-" + method + "\r\n";
    raw += route;
    raw += "From: <sip:alice@example.com>;tag=alice\r\nTo: <sip:+442071234567@example.com>;tag=carrier\r\n";
    raw += "Call-ID: call-proxy\r\nCSeq: " + std::to_string(cseq) + " " + method + "\r\nMax-Forwards: 70\r\n\r\n";
    return raw;
  };

  f.receive(f.caller, in_dialog("ACK", 1));
  const auto acks = f.to_carrier("ACK");
  ASSERT_EQ(acks.size(), 2u) << "the 407's ACK, then the 2xx's";
  EXPECT_EQ(cseq_of(acks[1]), 2u) << "an ACK carries its INVITE's CSeq (RFC 3261 13.2.2.4)";

  f.receive(f.caller, in_dialog("BYE", 2));
  const auto byes = f.to_carrier("BYE");
  ASSERT_EQ(byes.size(), 1u) << "the BYE goes out though the dialog has ended in the tracker";
  EXPECT_EQ(cseq_of(byes[0]), 3u) << "higher than the carrier's INVITE (RFC 3261 12.2.1.1)";

  f.receive(f.carrier, TrunkFixture::reply(byes[0], 200, "OK"));
  std::shared_ptr<SIPMessage> bye_answer;
  for (const auto& message : CoreFixture::written(f.caller_connection)) {
    if (message->header->type == SIPHeader::Type::Response && first(message, "CSeq").find("BYE") != std::string::npos) bye_answer = message;
  }
  ASSERT_NE(bye_answer, nullptr);
  EXPECT_EQ(cseq_of(bye_answer), 2u);
}

// Credentials the carrier refuses are not tried again, and the caller is not asked for a password it does not have.
TEST(ProxyTrunkTest, ASecondChallengeIsNotAnsweredAgain) {
  TrunkFixture f;

  f.receive(f.caller, f.invite_to_number());
  f.receive(f.carrier, TrunkFixture::reply(f.to_carrier("INVITE")[0], 407, "Proxy Authentication Required", kChallenge));
  f.receive(f.carrier, TrunkFixture::reply(f.to_carrier("INVITE").back(), 407, "Proxy Authentication Required", kChallenge));

  EXPECT_EQ(f.to_carrier("INVITE").size(), 2u);
  EXPECT_EQ(ProxyFixture::response_with(f.caller_connection, 407), nullptr);
  EXPECT_NE(ProxyFixture::response_with(f.caller_connection, 403), nullptr);
}

// A branch that is not a trunk passes a challenge upstream, as RFC 3261 16.7 step 7 has a proxy do.
TEST(ProxyTrunkTest, AChallengeFromSomethingThatIsNotATrunkGoesToTheCaller) {
  ProxyFixture f;
  f.bind_bob();

  f.receive(f.caller, f.invite());
  f.receive(f.callee, f.response_from_callee(407, "Proxy Authentication Required", "bob", "", kChallenge));

  EXPECT_NE(ProxyFixture::response_with(f.caller_connection, 407), nullptr);
}

// Caller ID for the carrier: the From's user and display changed and an asserted identity added on the copy that
// leaves, the From's tag kept, and the caller's own request untouched (RFC 3325 9.1).
TEST(ProxyTrunkTest, ThePolicySetsTheCallerIdTheCarrierSees) {
  TrunkFixture f;
  policy::HeaderEdit from{policy::HeaderEdit::Op::From, "From", {}};
  from.user = "+442012345678";
  from.display = "Reception";
  f.policy->edits = {from, policy::HeaderEdit{policy::HeaderEdit::Op::Set, "P-Asserted-Identity", "<sip:+442012345678@example.com>"},
                     policy::HeaderEdit{policy::HeaderEdit::Op::Remove, "User-Agent", {}}};

  auto invite = f.invite_to_number();
  invite.insert(invite.find("\r\n") + 2, "User-Agent: Alice's phone\r\n");
  f.receive(f.caller, invite);

  const auto sent = f.to_carrier("INVITE");
  ASSERT_EQ(sent.size(), 1u);
  const auto from_header = first(sent[0], "From");
  EXPECT_NE(from_header.find("sip:+442012345678@example.com"), std::string::npos) << from_header;
  EXPECT_NE(from_header.find("Reception"), std::string::npos) << from_header;
  EXPECT_NE(from_header.find("tag=alice"), std::string::npos) << "the tag names the dialog: " << from_header;
  EXPECT_EQ(first(sent[0], "P-Asserted-Identity"), "<sip:+442012345678@example.com>");
  EXPECT_FALSE(sent[0]->header->contains("User-Agent"));
}

// The call record says which trunk each leg ran over: a call that left by a trunk has it on the callee's leg.
TEST(ProxyTrunkTest, TheCallRecordNamesTheTrunkACallLeftBy) {
  TrunkFixture f;

  f.receive(f.caller, f.invite_to_number());
  f.receive(f.carrier, TrunkFixture::reply(f.to_carrier("INVITE")[0], 200, "OK", "Contact: <sip:+442071234567@192.0.2.30:5060>\r\n"));

  const auto legs = f.on_strand([&f]() {
    std::vector<std::pair<bool, std::string>> out;
    if (auto call = f.core->call_get("call-proxy")) {
      for (const auto& participant : call->participants) out.emplace_back(participant.originator, participant.trunk);
    }
    return out;
  });
  ASSERT_EQ(legs.size(), 2u);
  for (const auto& [originator, trunk] : legs) EXPECT_EQ(trunk, originator ? "" : "acme") << (originator ? "the caller's leg" : "the callee's leg");
}

// A call that came in by a trunk has it on the caller's leg.
TEST(ProxyTrunkTest, TheCallRecordNamesTheTrunkACallCameInBy) {
  TrunkFixture f;
  f.policy->trust_carrier = true;

  auto raw = f.invite("z9hG4bK-in", "sip:+442071234567@example.com");
  f.receive(f.carrier, raw);
  const auto out = f.to_carrier("INVITE");
  ASSERT_FALSE(out.empty());
  f.receive(f.carrier, TrunkFixture::reply(out.back(), 200, "OK", "Contact: <sip:+442071234567@192.0.2.30:5060>\r\n"));

  const auto caller_trunk = f.on_strand([&f]() {
    auto call = f.core->call_get("call-proxy");
    const auto index = call ? call->participant_index(true) : std::nullopt;
    return index ? call->participants[*index].trunk : std::string("(no call)");
  });
  EXPECT_EQ(caller_trunk, "acme");
}
