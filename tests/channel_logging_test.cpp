//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <boost/asio/ssl/error.hpp>
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

// A peer that closes a TLS connection without sending close_notify - which is what a browser
// does with a secure WebSocket when its page hangs up, and what plenty of phones do - has
// closed the connection. RFC 8446 6.1 has the receiver treat that as the end of the data,
// and here there is no message it could have truncated. It is a disconnect, not a fault,
// and logging it as an error on every call hides the errors that are.
TEST(ChannelLoggingTest, APeerClosingTlsWithoutCloseNotifyIsADisconnectNotAnError) {
  LoggingFixture f(false);

  std::shared_ptr<MockConnection> connection;
  auto channel = f.make_channel("198.51.100.90", &connection, "wss", 50000);
  ASSERT_TRUE(static_cast<bool>(connection->pending_read)) << "the channel is reading";

  auto read = connection->pending_read;
  f.on_strand([&read]() { read(boost::asio::ssl::error::stream_truncated, 0); });
  f.settle();

  for (const auto& line : f.logger->lines(athenasip::loggers::LogLevel::ERROR)) ADD_FAILURE() << line;

  bool disconnected = false;
  for (const auto& line : f.logger->lines(athenasip::loggers::LogLevel::INFO)) {
    if (line.find("Remote Disconnected") != std::string::npos) disconnected = true;
  }
  EXPECT_TRUE(disconnected);
}
