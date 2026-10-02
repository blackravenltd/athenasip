//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

#include "../helpers/core_fixture_helper.h"
#include "servers/tcp_server.h"
#include "servers/tls_server.h"
#include "servers/udp_server.h"

using namespace athenasip;

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using udp = boost::asio::ip::udp;

namespace {

const std::string kCert = std::string(ATHENA_TEST_SOURCE_DIR) + "/tls/snakeoil.cer";
const std::string kKey = std::string(ATHENA_TEST_SOURCE_DIR) + "/tls/snakeoil.key";

// RFC 3261 section 18 is about what the transport layer does with bytes, so these tests
// are written at that boundary: a real listener on a real socket, and a client that
// writes exactly the bytes the test means to send.
//
// The observable throughout is a 401. A REGISTER with no credentials has to be
// challenged (section 10.3 step 6), so a challenge coming back is proof that the bytes
// were framed into a message, reached the transaction layer and were answered down the
// same flow. Silence is proof the message was never assembled.

// A REGISTER with an optional body. The body is not meaningful to a registrar; it is
// here because Content-Length framing is what these tests are about.
std::string register_request(const std::string& transport, const std::string& branch, const std::string& body = "") {
  std::string raw = "REGISTER sip:example.com SIP/2.0\r\n";
  raw += "Via: SIP/2.0/" + transport + " 127.0.0.1:9;branch=" + branch + "\r\n";
  raw += "From: <sip:alice@example.com>;tag=alice\r\n";
  raw += "To: <sip:alice@example.com>\r\n";
  raw += "Call-ID: " + branch + "\r\n";
  raw += "CSeq: 1 REGISTER\r\n";
  raw += "Contact: <sip:alice@127.0.0.1:9>\r\n";
  raw += "Max-Forwards: 70\r\n";
  raw += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  raw += "\r\n";
  raw += body;
  return raw;
}

// A request whose Content-Length is a deliberate lie, for the two 18.3 rules about a
// datagram that does not carry the body it claims.
std::string register_claiming(const std::string& branch, std::size_t claimed, const std::string& body) {
  std::string raw = "REGISTER sip:example.com SIP/2.0\r\n";
  raw += "Via: SIP/2.0/UDP 127.0.0.1:9;branch=" + branch + "\r\n";
  raw += "From: <sip:alice@example.com>;tag=alice\r\n";
  raw += "To: <sip:alice@example.com>\r\n";
  raw += "Call-ID: " + branch + "\r\n";
  raw += "CSeq: 1 REGISTER\r\n";
  raw += "Contact: <sip:alice@127.0.0.1:9>\r\n";
  raw += "Max-Forwards: 70\r\n";
  raw += "Content-Length: " + std::to_string(claimed) + "\r\n";
  raw += "\r\n";
  raw += body;
  return raw;
}

int responses_in(const std::string& text) {
  int count = 0;
  std::size_t at = 0;

  while ((at = text.find("SIP/2.0 ", at)) != std::string::npos) {
    ++count;
    at += 8;
  }

  return count;
}

// Whatever arrives within the deadline, bounded rather than blocking: half of these
// tests are about a message that must never be answered, and a test that proves that by
// hanging is not a test.
//
// One read is outstanding at a time and the reader owns it across calls. Two overlapping
// reads on one socket deliver to whichever handler asio picks, which looked exactly like
// the node failing to answer.
template <typename Stream>
struct Reader {
  net::io_context& io;
  Stream& stream;
  std::string collected;
  std::array<char, 65535> buffer{};
  bool armed = false;

  Reader(net::io_context& context, Stream& s) : io(context), stream(s) {}

  ~Reader() {
    boost::system::error_code ec;
    stream.lowest_layer().cancel(ec);
    io.restart();
    io.run_for(std::chrono::milliseconds(50));
  }

  // Everything that arrived since the last call. Returns as soon as `wanted` responses
  // are in, so a test that expects an answer costs what the answer costs rather than
  // what the deadline is.
  std::string take(std::chrono::milliseconds wait, int wanted = 1) {
    _wanted = wanted;
    _arm();
    io.restart();
    io.run_for(wait);

    auto out = collected;
    collected.clear();
    return out;
  }

 private:
  void _arm() {
    if (armed) return;
    armed = true;

    _start([this](const boost::system::error_code& ec, std::size_t length) {
      armed = false;
      if (ec) return;

      collected.append(buffer.data(), length);

      if (_wanted > 0 && responses_in(collected) >= _wanted) return io.stop();

      _arm();
    });
  }

  template <typename Handler>
  void _start(Handler handler) {
    if constexpr (std::is_same_v<Stream, udp::socket>) {
      stream.async_receive_from(net::buffer(buffer), _from, std::move(handler));
    } else {
      stream.async_read_some(net::buffer(buffer), std::move(handler));
    }
  }

  udp::endpoint _from;
  int _wanted = 1;
};

struct TcpFixture : CoreFixture {
  std::shared_ptr<servers::TCPServer> server;

  TcpFixture() {
    seed_realm("example.com");
    server = std::make_shared<servers::TCPServer>(logger, core, "127.0.0.1", 0);
    server->start();
  }

  ~TcpFixture() {
    on_strand([this]() { core->channel_close_all(); });
    settle();
    server->stop();
  }

  std::uint16_t port() const { return server->port(); }
};

struct TlsFixture : CoreFixture {
  std::shared_ptr<servers::TLSServer> server;

  TlsFixture() {
    seed_realm("example.com");
    server = std::make_shared<servers::TLSServer>(logger, core, "127.0.0.1", 0);
    EXPECT_TRUE(server->set_certificates(kCert, kKey));
    server->start();
  }

  ~TlsFixture() {
    on_strand([this]() { core->channel_close_all(); });
    settle();
    server->stop();
  }

  std::uint16_t port() const { return server->port(); }
};

struct UdpFixture : CoreFixture {
  std::shared_ptr<servers::UDPServer> server;

  UdpFixture() {
    seed_realm("example.com");
    server = std::make_shared<servers::UDPServer>(logger, core, "127.0.0.1", 0);
    server->start();
  }

  ~UdpFixture() {
    on_strand([this]() { core->channel_close_all(); });
    settle();
    server->stop();
  }

  std::uint16_t port() { return server->local_endpoint().port(); }
};

}  // namespace

// The baseline for the stream transports: bytes in, a challenge back. Without this the
// framing tests below would be asserting about a listener that never worked.
TEST(TransportFramingTest, TcpCarriesARequestAndAnswersItDownTheSameConnection) {
  TcpFixture f;

  net::io_context io;
  tcp::socket socket(io);
  socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  Reader<tcp::socket> reader(io, socket);

  net::write(socket, net::buffer(register_request("TCP", "z9hG4bK-tcp-plain")));

  const auto answer = reader.take(std::chrono::seconds(3));
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// RFC 3261 18.3: on a stream transport the Content-Length field is what says where the
// body ends. TCP is a stream and not a record protocol, so a message may arrive in as
// many pieces as the network cares to use, and the transport has to put it back
// together rather than parse whatever one read happened to deliver.
TEST(TransportFramingTest, TcpReassemblesARequestSplitAcrossWrites) {
  TcpFixture f;

  net::io_context io;
  tcp::socket socket(io);
  socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  Reader<tcp::socket> reader(io, socket);

  const auto request = register_request("TCP", "z9hG4bK-tcp-split", "v=0\r\n");
  const auto cut = request.size() / 2;

  net::write(socket, net::buffer(request.substr(0, cut)));

  // Nothing may be answered yet: half a message is not a message.
  EXPECT_EQ(reader.take(std::chrono::milliseconds(300), 0), "");

  net::write(socket, net::buffer(request.substr(cut)));

  const auto answer = reader.take(std::chrono::seconds(3));
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// The other half of the same rule. A stream has no message boundaries, so two requests
// written together arrive as one read and both have to come out of it.
TEST(TransportFramingTest, TcpTakesTwoRequestsFromOneWrite) {
  TcpFixture f;

  net::io_context io;
  tcp::socket socket(io);
  socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  Reader<tcp::socket> reader(io, socket);

  const auto both = register_request("TCP", "z9hG4bK-tcp-first") + register_request("TCP", "z9hG4bK-tcp-second");
  net::write(socket, net::buffer(both));

  const auto answer = reader.take(std::chrono::seconds(3), 2);
  EXPECT_EQ(responses_in(answer), 2) << answer;
}

// Content-Length and nothing else says where a body ends. A transport that looked for
// the next start line instead would swallow the second request whenever the first one
// carried a body, which is every INVITE.
TEST(TransportFramingTest, TcpUsesContentLengthToFindTheEndOfABody) {
  TcpFixture f;

  net::io_context io;
  tcp::socket socket(io);
  socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  Reader<tcp::socket> reader(io, socket);

  const auto both =
      register_request("TCP", "z9hG4bK-tcp-body", "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n") + register_request("TCP", "z9hG4bK-tcp-after");

  net::write(socket, net::buffer(both));

  const auto answer = reader.take(std::chrono::seconds(3), 2);
  EXPECT_EQ(responses_in(answer), 2) << answer;
}

// A body that has not all arrived is a message that has not all arrived, whatever the
// headers say.
TEST(TransportFramingTest, TcpWaitsForTheWholeBodyBeforeAnswering) {
  TcpFixture f;

  net::io_context io;
  tcp::socket socket(io);
  socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  Reader<tcp::socket> reader(io, socket);

  const std::string body = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n";
  const auto request = register_request("TCP", "z9hG4bK-tcp-partial", body);
  const auto headers_and_half = request.size() - (body.size() / 2);

  net::write(socket, net::buffer(request.substr(0, headers_and_half)));
  EXPECT_EQ(reader.take(std::chrono::milliseconds(300), 0), "");

  net::write(socket, net::buffer(request.substr(headers_and_half)));

  const auto answer = reader.take(std::chrono::seconds(3));
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// The same rule as the datagram case and the reason it matters most: a start line that
// did not parse has no Request-URI, and anything that serialises the message throws.
// On a read handler that exception is the process. Whatever the node decides to answer,
// it has to still be there afterwards.
TEST(TransportFramingTest, TcpSurvivesAMalformedStartLine) {
  TcpFixture f;

  net::io_context io;
  tcp::socket socket(io);
  socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  Reader<tcp::socket> reader(io, socket);

  net::write(socket, net::buffer(std::string("this is not a request line\r\n\r\n")));
  net::write(socket, net::buffer(register_request("TCP", "z9hG4bK-tcp-after-rubbish")));

  // 400 for the rubbish (RFC 3261 8.2.1), and then the node is still there to challenge
  // the request that followed it.
  const auto answer = reader.take(std::chrono::seconds(3), 2);

  EXPECT_NE(answer.find("SIP/2.0 400"), std::string::npos) << answer;
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// RFC 5626 section 4.4.1: on a stream transport a client's keep-alive is a double CRLF,
// the "ping", and the server MUST answer it with a single CRLF, the "pong". A client that
// hears no pong decides its flow has failed and registers again, so a server that
// swallowed the ping would have every outbound client re-registering every few minutes.
TEST(TransportFramingTest, TcpAnswersAKeepAlivePingWithAPong) {
  TcpFixture f;

  net::io_context io;
  tcp::socket socket(io);
  socket.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  Reader<tcp::socket> reader(io, socket);

  net::write(socket, net::buffer(std::string("\r\n\r\n")));
  EXPECT_EQ(reader.take(std::chrono::milliseconds(500), 0), "\r\n");

  // The ping may arrive in two reads like anything else on a stream.
  net::write(socket, net::buffer(std::string("\r\n")));
  EXPECT_EQ(reader.take(std::chrono::milliseconds(300), 0), "") << "one CRLF is not a ping";
  net::write(socket, net::buffer(std::string("\r\n")));
  EXPECT_EQ(reader.take(std::chrono::milliseconds(500), 0), "\r\n");

  // And a request after it is still a request.
  net::write(socket, net::buffer(register_request("TCP", "z9hG4bK-after-ping")));
  const auto answer = reader.take(std::chrono::seconds(3));
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

TEST(TransportFramingTest, TlsCarriesARequestOverACompletedHandshake) {
  TlsFixture f;

  net::io_context io;
  ssl::context context(ssl::context::tls_client);
  context.set_verify_mode(ssl::verify_none);

  ssl::stream<tcp::socket> stream(io, context);
  stream.next_layer().connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), f.port()));
  stream.handshake(ssl::stream_base::client);
  Reader<ssl::stream<tcp::socket>> reader(io, stream);

  net::write(stream, net::buffer(register_request("TLS", "z9hG4bK-tls-plain")));

  const auto answer = reader.take(std::chrono::seconds(3));
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

TEST(TransportFramingTest, UdpCarriesADatagramAndAnswersItToTheSourcePort) {
  UdpFixture f;

  net::io_context io;
  udp::socket socket(io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0));
  const udp::endpoint node(net::ip::make_address("127.0.0.1"), f.port());
  Reader<udp::socket> reader(io, socket);

  socket.send_to(net::buffer(register_request("UDP", "z9hG4bK-udp-plain")), node);

  const auto answer = reader.take(std::chrono::seconds(3));
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// RFC 3261 18.3: "If the message has a Content-Length header field value that is greater
// than the size of the body, the message MUST be discarded" on a message-oriented
// transport. A datagram is the whole message or it is nothing: there is no later packet
// that completes it, and treating the next datagram from that source as the missing
// body makes one sender able to swallow another's request on a flow they share.
TEST(TransportFramingTest, UdpDiscardsADatagramThatDoesNotCarryTheBodyItClaims) {
  UdpFixture f;

  net::io_context io;
  udp::socket socket(io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0));
  const udp::endpoint node(net::ip::make_address("127.0.0.1"), f.port());
  Reader<udp::socket> reader(io, socket);

  socket.send_to(net::buffer(register_claiming("z9hG4bK-udp-liar", 5000, "v=0\r\n")), node);

  EXPECT_EQ(reader.take(std::chrono::milliseconds(500), 0), "");
}

// The same lie, and the consequence that makes it matter: the truncated datagram must
// not eat the next one. A listener that kept the incomplete message waiting would take
// the following request as its body and answer neither.
TEST(TransportFramingTest, ADiscardedDatagramDoesNotSwallowTheNextOne) {
  UdpFixture f;

  net::io_context io;
  udp::socket socket(io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0));
  const udp::endpoint node(net::ip::make_address("127.0.0.1"), f.port());
  Reader<udp::socket> reader(io, socket);

  socket.send_to(net::buffer(register_claiming("z9hG4bK-udp-liar-2", 5000, "v=0\r\n")), node);
  socket.send_to(net::buffer(register_request("UDP", "z9hG4bK-udp-good")), node);

  const auto answer = reader.take(std::chrono::seconds(3));

  EXPECT_EQ(responses_in(answer), 1) << answer;
  EXPECT_NE(answer.find("z9hG4bK-udp-good"), std::string::npos) << answer;
}

// The other half of 18.3: "If the message has a Content-Length header field value that
// is less than the size of the body, the body is truncated to that length." The extra
// bytes are not a second message and must not be read as one.
TEST(TransportFramingTest, UdpTruncatesABodyLongerThanItsContentLength) {
  UdpFixture f;

  net::io_context io;
  udp::socket socket(io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0));
  const udp::endpoint node(net::ip::make_address("127.0.0.1"), f.port());
  Reader<udp::socket> reader(io, socket);

  auto datagram = register_claiming("z9hG4bK-udp-long", 5, "v=0\r\nand a great deal more that nobody asked for\r\n");
  socket.send_to(net::buffer(datagram), node);

  const auto answer = reader.take(std::chrono::seconds(3));

  EXPECT_EQ(responses_in(answer), 1) << answer;
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// A listener that stopped on the first thing it could not parse would be a listener
// anybody could turn off with one packet.
TEST(TransportFramingTest, UdpKeepsListeningAfterADatagramThatIsNotSip) {
  UdpFixture f;

  net::io_context io;
  udp::socket socket(io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0));
  const udp::endpoint node(net::ip::make_address("127.0.0.1"), f.port());
  Reader<udp::socket> reader(io, socket);

  socket.send_to(net::buffer(std::string("this is not a SIP message at all")), node);
  socket.send_to(net::buffer(register_request("UDP", "z9hG4bK-udp-after-rubbish")), node);

  const auto answer = reader.take(std::chrono::seconds(3));
  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;
}

// One flow per peer, not one per datagram. RFC 3261 18.2.1 has responses go back to the
// source of the request, and the channel registry is keyed by transport, address and
// port, so a second request from the same socket has to find the flow the first one
// made rather than open another.
TEST(TransportFramingTest, TwoDatagramsFromOnePeerShareOneFlow) {
  UdpFixture f;

  net::io_context io;
  udp::socket socket(io, udp::endpoint(net::ip::make_address("127.0.0.1"), 0));
  const udp::endpoint node(net::ip::make_address("127.0.0.1"), f.port());
  Reader<udp::socket> reader(io, socket);

  socket.send_to(net::buffer(register_request("UDP", "z9hG4bK-udp-one")), node);
  reader.take(std::chrono::seconds(2));

  socket.send_to(net::buffer(register_request("UDP", "z9hG4bK-udp-two")), node);
  const auto answer = reader.take(std::chrono::seconds(3));

  EXPECT_NE(answer.find("SIP/2.0 401"), std::string::npos) << answer;

  const auto source = "udp://127.0.0.1:" + std::to_string(socket.local_endpoint().port());
  EXPECT_NE(f.on_strand([&]() { return f.core->channel_find(source); }), nullptr);
}
