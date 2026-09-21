//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
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

#include "servers/websocket_server.h"
#include "sip_header.h"

#include "../helpers/core_fixture_helper.h"

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

// A node with one WebSocket listener on a port the operating system picked, so nothing
// here depends on a number being free. Port zero is what a test binds; a node binds what
// it was configured with.
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

  // A REGISTER with no credentials. The registrar has to challenge it, which is what
  // proves the bytes that arrived over the WebSocket reached the transaction layer and
  // an answer came back the same way.
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

// RFC 7118 section 4: the client offers the "sip" subprotocol and the server names it
// back. A client that does not see it back is entitled to conclude the far end does not
// speak SIP.
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

// A browser will not open an insecure WebSocket from a page served over https, so wss is
// not a hardening option for a web client but the only way in. This is the whole of the
// path: TLS handshake, HTTP upgrade, SIP over the frames, and an answer back.
TEST(WebsocketServerTest, CarriesSipOverTls) {
  WebsocketFixture fixture(true);

  net::io_context io;
  ssl::context context(ssl::context::tls_client);

  // The snakeoil certificate is self-signed and names nothing this test resolves. What is
  // under test is that the listener speaks TLS at all, not that a CA vouches for it.
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

// ws:// is the same path with nothing under it, and stays for local development. The two
// listeners are one class, so this is what says the TLS one added a layer rather than
// changing the behaviour.
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

// A secure listener that would serve a client which never offered TLS is not a secure
// listener. The handshake has to fail rather than fall back.
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

// The listener is not a web server. Anything that is not the SIP upgrade is answered and
// closed, not upgraded.
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

// Both files or neither. A listener asked to be secure with nothing to be secure with
// must refuse to start rather than quietly serve a browser in the clear.
TEST(WebsocketServerTest, RefusesToBeSecureWithoutBothFiles) {
  WebsocketFixture fixture(false);

  EXPECT_FALSE(fixture.server->set_certificates("", kKey));
  EXPECT_FALSE(fixture.server->set_certificates(kCert, ""));
  EXPECT_FALSE(fixture.server->set_certificates(std::string(ATHENA_TEST_SOURCE_DIR) + "/tls/nosuch.cer", kKey));
}
