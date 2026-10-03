//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <string>

#include "helpers/proxy_fixture_helper.h"

using namespace athenasip;

namespace {

const std::string kOffer =
    "v=0\r\n"
    "o=alice 1 1 IN IP4 192.0.2.10\r\n"
    "s=-\r\n"
    "c=IN IP4 192.0.2.10\r\n"
    "t=0 0\r\n"
    "m=audio 49170 RTP/AVP 0\r\n";

struct LoggingFixture : ProxyFixture {
  explicit LoggingFixture(bool log_messages) {
    config->sip_log_messages = log_messages;
    bind_bob();
  }

  std::string invite_with_body() {
    auto raw = invite();
    raw.insert(raw.size() - 2, "Content-Type: application/sdp\r\nContent-Length: " + std::to_string(kOffer.size()) + "\r\n");
    return raw + kOffer;
  }

  // A REGISTER carrying credentials, which is the thing that must not end up in a log
  // whatever else does.
  std::string register_with_credentials() {
    std::string raw = "REGISTER sip:example.com SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-register\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:alice@example.com>\r\n";
    raw += "Call-ID: call-register\r\n";
    raw += "CSeq: 1 REGISTER\r\n";
    raw += "Contact: <sip:alice@192.0.2.10:5060>\r\n";
    raw +=
        "Authorization: Digest username=\"alice\", realm=\"example.com\", nonce=\"abc\", uri=\"sip:example.com\", "
        "response=\"deadbeefdeadbeefdeadbeefdeadbeef\"\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "\r\n";
    return raw;
  }
};

}  // namespace

// One line per message is the readable trace, and it is what a node logs by default:
// every message in and out, named by its first line.
TEST(ChannelLoggingTest, TheSummaryOfEveryMessageIsAlwaysLogged) {
  LoggingFixture f(false);

  f.receive(f.caller, f.invite_with_body());

  const auto text = f.logger->text();
  EXPECT_NE(text.find("> INVITE sip:bob@example.com"), std::string::npos) << text;
}

// And a body is not in it. A description is the one thing a node is better placed to
// show than either end of the call, but it is also most of the bytes, so it is asked
// for rather than assumed.
TEST(ChannelLoggingTest, ABodyIsNotLoggedUnlessItWasAskedFor) {
  LoggingFixture f(false);

  f.receive(f.caller, f.invite_with_body());

  EXPECT_EQ(f.logger->text().find("m=audio"), std::string::npos);
}

// With it asked for, the whole message is there: the description this node was handed
// and the one it produced, which is what the interop runbook otherwise has to read off
// the two endpoints.
TEST(ChannelLoggingTest, TheWholeMessageIsLoggedWhenItWasAskedFor) {
  LoggingFixture f(true);

  f.receive(f.caller, f.invite_with_body());

  const auto text = f.logger->text();

  EXPECT_NE(text.find("m=audio 49170 RTP/AVP 0"), std::string::npos) << text;
  EXPECT_NE(text.find("o=alice 1 1 IN IP4 192.0.2.10"), std::string::npos) << text;

  // The headers travel with it, or the body cannot be attributed to a message.
  EXPECT_NE(text.find("Call-ID: call-proxy"), std::string::npos) << text;
}

// Credentials are the exception, and they are redacted whichever direction they were
// going. A Digest response is a hash rather than the password, but it is replayable
// for as long as its nonce lives, and a log is a file somebody else can read.
TEST(ChannelLoggingTest, CredentialsAreRedactedFromWhatIsLogged) {
  LoggingFixture f(true);

  f.receive(f.caller, f.register_with_credentials());

  const auto text = f.logger->text();

  EXPECT_EQ(text.find("deadbeefdeadbeefdeadbeefdeadbeef"), std::string::npos) << text;
  EXPECT_NE(text.find("Authorization: <redacted>"), std::string::npos) << text;

  // The challenge this node sent back is redacted too: it carries the nonce the next
  // response is computed over.
  EXPECT_EQ(text.find("WWW-Authenticate: Digest realm"), std::string::npos) << text;
}
