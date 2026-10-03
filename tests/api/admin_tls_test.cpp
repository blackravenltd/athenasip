//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <filesystem>
#include <memory>
#include <string>

#include "../mocks/logger_mock.h"
#include "api/admin_api.h"
#include "cluster_ca.h"

using namespace athenasip;

namespace {

namespace beast = boost::beast;
namespace http = boost::beast::http;
namespace net = boost::asio;

// The admin server with a certificate, made the way a cluster's are so the test needs no
// file checked in: an authority, and a certificate it signed naming 127.0.0.1.
struct TlsFixture {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::filesystem::path dir;
  std::shared_ptr<api::AdminAPI> admin;
  std::string remote_seen;

  TlsFixture() {
    dir = std::filesystem::temp_directory_path() / ("athenasip-admin-tls-" + std::to_string(::getpid()) + "-" + std::to_string(counter()++));
    std::filesystem::create_directories(dir);

    EXPECT_TRUE(ca::init(dir.string()).ok);
    EXPECT_TRUE(ca::issue_node(dir.string(), "admin", {"127.0.0.1"}).ok);

    admin = std::make_shared<api::AdminAPI>(logger, "127.0.0.1", 0);
    admin->middlewares.push_back([this](const http::request<http::string_body>&, const std::string& remote,
                                        std::shared_ptr<http::response<http::string_body>> response, std::function<void(bool)> next) {
      remote_seen = remote;
      response->result(http::status::ok);
      response->body() = "served";
      next(false);
    });
  }

  ~TlsFixture() {
    admin->stop();
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
  }

  static int& counter() {
    static int value = 0;
    return value;
  }

  std::string cert() const { return (dir / "admin.crt").string(); }
  std::string key() const { return (dir / "admin.key").string(); }

  std::string get_plain() {
    net::io_context io;
    net::ip::tcp::socket socket(io);
    socket.connect({net::ip::make_address("127.0.0.1"), admin->port()});
    return exchange(socket);
  }

  // Over TLS, trusting only the authority that signed the server's certificate and
  // checking it names the address dialled, as a browser would.
  std::string get_tls(boost::system::error_code& error) {
    net::io_context io;
    net::ssl::context context(net::ssl::context::tls_client);
    context.load_verify_file((dir / "ca.crt").string());
    context.set_verify_mode(net::ssl::verify_peer);

    net::ssl::stream<net::ip::tcp::socket> stream(io, context);
    stream.set_verify_callback(net::ssl::host_name_verification("127.0.0.1"));
    stream.next_layer().connect({net::ip::make_address("127.0.0.1"), admin->tls_port()});

    stream.handshake(net::ssl::stream_base::client, error);
    if (error) return "";

    return exchange(stream);
  }

  template <typename Stream>
  static std::string exchange(Stream& stream) {
    http::request<http::string_body> request{http::verb::get, "/", 11};
    request.set(http::field::host, "127.0.0.1");
    request.prepare_payload();
    http::write(stream, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    boost::system::error_code ec;
    http::read(stream, buffer, response, ec);
    return response.body();
  }
};

}  // namespace

// A browser gives a page that is not a secure context no microphone, so the console's
// softphone cannot place a call from plain HTTP (found on 2026-10-03). The admin listener
// offers HTTPS, with the certificate it was given, serving the same chain.
TEST(AdminTlsTest, TheSameChainIsServedOverHttps) {
  TlsFixture f;
  ASSERT_TRUE(f.admin->tls_enable("127.0.0.1", 0, f.cert(), f.key()));
  f.admin->start();

  ASSERT_NE(f.admin->tls_port(), 0);

  boost::system::error_code error;
  EXPECT_EQ(f.get_tls(error), "served");
  EXPECT_FALSE(error) << error.message();

  // Who is calling is known over TLS as it is over plain HTTP: the rate limits are by it.
  EXPECT_EQ(f.remote_seen, "127.0.0.1");
}

// And plain HTTP stays, beside it: a healthcheck and a provisioning script on the host
// have no certificate to check and no reason to need one (Tom, 2026-10-03).
TEST(AdminTlsTest, PlainHttpIsStillServedBesideIt) {
  TlsFixture f;
  ASSERT_TRUE(f.admin->tls_enable("127.0.0.1", 0, f.cert(), f.key()));
  f.admin->start();

  EXPECT_EQ(f.get_plain(), "served");
  EXPECT_NE(f.admin->port(), f.admin->tls_port());
}

// Off unless asked for, and then there is no second listener at all.
TEST(AdminTlsTest, WithoutACertificateThereIsNoHttpsListener) {
  TlsFixture f;
  EXPECT_FALSE(f.admin->tls_enable("127.0.0.1", 0, "/nowhere/cert.pem", "/nowhere/key.pem"));
  f.admin->start();

  EXPECT_EQ(f.admin->tls_port(), 0);
  EXPECT_EQ(f.get_plain(), "served");
}

// One connection that opens and never finishes its handshake holds up nobody else.
TEST(AdminTlsTest, ASilentConnectionDoesNotStopTheNextOne) {
  TlsFixture f;
  ASSERT_TRUE(f.admin->tls_enable("127.0.0.1", 0, f.cert(), f.key()));
  f.admin->start();

  net::io_context io;
  net::ip::tcp::socket silent(io);
  silent.connect({net::ip::make_address("127.0.0.1"), f.admin->tls_port()});

  boost::system::error_code error;
  EXPECT_EQ(f.get_tls(error), "served");
  EXPECT_FALSE(error) << error.message();
}
