//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <memory>

#include "servers/tcp_connection.h"
#include "servers/tls_connection.h"
#include "servers/websocket_connection.h"

using namespace athenasip::servers;

namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;

// A peer may reset its connection at any moment, including between the accept and the
// connection being made from it, and asking such a socket for its peer's address fails.
// That failure was an exception thrown inside the listener's handler, which nothing caught:
// the whole node stopped. A socket with no peer is what such a socket looks like, and the
// connection made from it has to be a connection that is simply not open.
TEST(ConnectionEndpointTest, ATcpConnectionWhosePeerHasGoneDoesNotThrow) {
  net::io_context io;
  auto socket = std::make_shared<tcp::socket>(io);
  socket->open(tcp::v4());

  EXPECT_NO_THROW({ TCPConnection connection(socket); });
}

TEST(ConnectionEndpointTest, ATlsConnectionWhosePeerHasGoneDoesNotThrow) {
  net::io_context io;
  net::ssl::context context(net::ssl::context::tls_server);
  auto stream = std::make_shared<net::ssl::stream<tcp::socket>>(io, context);
  stream->lowest_layer().open(tcp::v4());

  EXPECT_NO_THROW({ TLSConnection connection(stream, true); });
}

TEST(ConnectionEndpointTest, AWebsocketConnectionWhosePeerHasGoneDoesNotThrow) {
  net::io_context io;
  auto ws = std::make_unique<boost::beast::websocket::stream<tcp::socket>>(io);
  boost::beast::get_lowest_layer(*ws).open(tcp::v4());

  auto connection = std::make_shared<WebsocketConnectionFor<tcp::socket>>(std::move(ws), "ws");
  EXPECT_NO_THROW(connection->start());
}
