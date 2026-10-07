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

  // A REGISTER carrying Digest credentials, which must never reach a log.
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

// By default every message in and out is logged as one line: its first line.
TEST(ChannelLoggingTest, TheSummaryOfEveryMessageIsAlwaysLogged) {
  LoggingFixture f(false);

  f.receive(f.caller, f.invite_with_body());

  const auto text = f.logger->text();
  EXPECT_NE(text.find("> INVITE sip:bob@example.com"), std::string::npos) << text;
}

// Bodies are logged only when sip_log_messages is set.
TEST(ChannelLoggingTest, ABodyIsNotLoggedUnlessItWasAskedFor) {
  LoggingFixture f(false);

  f.receive(f.caller, f.invite_with_body());

  EXPECT_EQ(f.logger->text().find("m=audio"), std::string::npos);
}

// With sip_log_messages set, the whole message is logged, headers and body.
TEST(ChannelLoggingTest, TheWholeMessageIsLoggedWhenItWasAskedFor) {
  LoggingFixture f(true);

  f.receive(f.caller, f.invite_with_body());

  const auto text = f.logger->text();

  EXPECT_NE(text.find("m=audio 49170 RTP/AVP 0"), std::string::npos) << text;
  EXPECT_NE(text.find("o=alice 1 1 IN IP4 192.0.2.10"), std::string::npos) << text;

  // The headers are logged too, so the body can be attributed to its message.
  EXPECT_NE(text.find("Call-ID: call-proxy"), std::string::npos) << text;
}

// Credentials are redacted in both directions: a Digest response is replayable while its nonce lives.
TEST(ChannelLoggingTest, CredentialsAreRedactedFromWhatIsLogged) {
  LoggingFixture f(true);

  f.receive(f.caller, f.register_with_credentials());

  const auto text = f.logger->text();

  EXPECT_EQ(text.find("deadbeefdeadbeefdeadbeefdeadbeef"), std::string::npos) << text;
  EXPECT_NE(text.find("Authorization: <redacted>"), std::string::npos) << text;

  // The challenge sent back is redacted too: it carries the nonce.
  EXPECT_EQ(text.find("WWW-Authenticate: Digest realm"), std::string::npos) << text;
}

// A TLS peer closing without close_notify (browsers and many phones do) is logged as a disconnect,
// not an error (RFC 8446 6.1).
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
