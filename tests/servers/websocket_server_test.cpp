//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "servers/websocket_server.h"

#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../helpers/core_fixture_helper.h"
#include "sip_header.h"

using namespace athenasip;

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;

namespace {

const std::string kCert = std::string(ATHENA_TEST_SOURCE_DIR) + "/tls/snakeoil.cer";
const std::string kKey = std::string(ATHENA_TEST_SOURCE_DIR) + "/tls/snakeoil.key";

// A node with one WebSocket listener on a port the operating system picked.
struct WebsocketFixture : CoreFixture {
  std::shared_ptr<athenasip::servers::WebsocketServer> server;

  explicit WebsocketFixture(bool tls) {
    seed_realm("example.com");

    server = std::make_shared<athenasip::servers::WebsocketServer>(logger, core, "127.0.0.1", 0);
    if (tls) EXPECT_TRUE(server->set_certificates(kCert, kKey));

    server->start();
  }

  ~WebsocketFixture() {
    on_strand([this]() { core->channel_close_all(); });
    settle();
    server->stop();
  }

  std::uint16_t port() const { return server->port(); }
  std::string authority() const { return "127.0.0.1:" + std::to_string(port()); }

  // A REGISTER with no credentials. The challenge proves the bytes reached the transaction layer and an answer
  // came back the same way.
  static std::string register_request() {
    std::string raw = "REGISTER sip:example.com SIP/2.0\r\n";
    raw += "Via: SIP/2.0/WSS df7jal23ls0d.invalid;branch=z9hG4bK-ws-register\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:alice@example.com>\r\n";
    raw += "Call-ID: call-websocket\r\n";
    raw += "CSeq: 1 REGISTER\r\n";
    raw += "Contact: <sip:alice@df7jal23ls0d.invalid;transport=ws>\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "Content-Length: 0\r\n";
    raw += "\r\n";
    return raw;
  }
};

// RFC 7118 4: the client offers the "sip" subprotocol and the server names it back.
void offer_sip_subprotocol(websocket::request_type& request) { request.set(http::field::sec_websocket_protocol, "sip"); }

std::string read_one(auto& ws) {
  beast::flat_buffer buffer;
  ws.read(buffer);
  return beast::buffers_to_string(buffer.data());
}

int response_code_of(const std::string& raw) {
  const auto split = raw.find("\r\n\r\n");
  SIPHeader header(split == std::string::npos ? raw : raw.substr(0, split));
  return header.response_code;
}

}  // namespace

// wss is the only way in for a browser on an https page. The whole path: TLS handshake, HTTP upgrade, SIP over
// the frames, and an answer back.
TEST(WebsocketServerTest, CarriesSipOverTls) {
  WebsocketFixture fixture(true);

  net::io_context io;
  ssl::context context(ssl::context::tls_client);

  // The test certificate is self-signed. What is under test is that the listener speaks TLS, not who vouches for it.
  context.set_verify_mode(ssl::verify_none);

  websocket::stream<ssl::stream<tcp::socket>> ws(io, context);

  net::connect(beast::get_lowest_layer(ws), std::vector<tcp::endpoint>{tcp::endpoint(net::ip::make_address("127.0.0.1"), fixture.port())});
  ws.next_layer().handshake(ssl::stream_base::client);

  ws.set_option(websocket::stream_base::decorator(offer_sip_subprotocol));

  websocket::response_type response;
  ASSERT_NO_THROW(ws.handshake(response, fixture.authority(), "/"));
  EXPECT_EQ(response[http::field::sec_websocket_protocol], "sip");

  ws.write(net::buffer(WebsocketFixture::register_request()));
  EXPECT_EQ(response_code_of(read_one(ws)), 401);

  beast::error_code ec;
  ws.close(websocket::close_code::normal, ec);
}

// ws:// is the same path without TLS, for local development.
TEST(WebsocketServerTest, CarriesSipWithoutTls) {
  WebsocketFixture fixture(false);

  net::io_context io;
  websocket::stream<tcp::socket> ws(io);

  net::connect(ws.next_layer(), std::vector<tcp::endpoint>{tcp::endpoint(net::ip::make_address("127.0.0.1"), fixture.port())});

  ws.set_option(websocket::stream_base::decorator(offer_sip_subprotocol));

  websocket::response_type response;
  ASSERT_NO_THROW(ws.handshake(response, fixture.authority(), "/"));
  EXPECT_EQ(response[http::field::sec_websocket_protocol], "sip");

  ws.write(net::buffer(WebsocketFixture::register_request()));
  EXPECT_EQ(response_code_of(read_one(ws)), 401);

  beast::error_code ec;
  ws.close(websocket::close_code::normal, ec);
}

// A secure listener refuses a client that does not offer TLS, rather than falling back.
TEST(WebsocketServerTest, ASecureListenerRefusesAPlainClient) {
  WebsocketFixture fixture(true);

  net::io_context io;
  websocket::stream<tcp::socket> ws(io);

  net::connect(ws.next_layer(), std::vector<tcp::endpoint>{tcp::endpoint(net::ip::make_address("127.0.0.1"), fixture.port())});

  beast::error_code ec;
  websocket::response_type response;
  ws.handshake(response, fixture.authority(), "/", ec);

  EXPECT_TRUE(ec) << "a plain client completed a handshake with a wss listener";
}

// RFC 7118 names no path and clients pick their own ("/ws" is common). The listener serves nothing but SIP, so
// it accepts the upgrade on any path.
TEST(WebsocketServerTest, AcceptsTheUpgradeOnWhateverPathTheClientAsks) {
  WebsocketFixture fixture(false);

  for (const auto* target : {"/", "/ws", "/sip", "/ws?token=x"}) {
    net::io_context io;
    websocket::stream<tcp::socket> ws(io);

    net::connect(ws.next_layer(), std::vector<tcp::endpoint>{tcp::endpoint(net::ip::make_address("127.0.0.1"), fixture.port())});
    ws.set_option(websocket::stream_base::decorator(offer_sip_subprotocol));

    websocket::response_type response;
    ASSERT_NO_THROW(ws.handshake(response, fixture.authority(), target)) << target;
    EXPECT_EQ(response[http::field::sec_websocket_protocol], "sip") << target;

    beast::error_code ec;
    ws.close(websocket::close_code::normal, ec);
  }
}

TEST(WebsocketServerTest, RefusesARequestThatIsNotAnUpgrade) {
  WebsocketFixture fixture(false);

  net::io_context io;
  tcp::socket socket(io);
  net::connect(socket, std::vector<tcp::endpoint>{tcp::endpoint(net::ip::make_address("127.0.0.1"), fixture.port())});

  http::request<http::string_body> request(http::verb::get, "/", 11);
  request.set(http::field::host, fixture.authority());
  http::write(socket, request);

  beast::flat_buffer buffer;
  http::response<http::string_body> response;
  beast::error_code ec;
  http::read(socket, buffer, response, ec);

  ASSERT_FALSE(ec) << ec.message();
  EXPECT_EQ(response.result_int(), 400);
}

// Both files or neither: a secure listener with nothing to be secure with refuses to start.
TEST(WebsocketServerTest, RefusesToBeSecureWithoutBothFiles) {
  WebsocketFixture fixture(false);

  EXPECT_FALSE(fixture.server->set_certificates("", kKey));
  EXPECT_FALSE(fixture.server->set_certificates(kCert, ""));
  EXPECT_FALSE(fixture.server->set_certificates(std::string(ATHENA_TEST_SOURCE_DIR) + "/tls/nosuch.cer", kKey));
}
