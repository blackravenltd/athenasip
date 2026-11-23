//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// rediss:// is TLS that checks who answers: the server's certificate against the system's roots and redis.ca_file,
// and its name against the URL's host. Without that the password crosses the network to whoever is in the way.
#include <gtest/gtest.h>
#include <yaml-cpp/yaml.h>

#include <atomic>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "../mocks/logger_mock.h"
#include "config.h"
#include "datastores/redis_datastore.h"
#include "types/url.h"

using namespace athenasip;
using athenasip::datastores::RedisDatastore;

namespace {

std::string cert_file() { return std::string(ATHENA_TEST_SOURCE_DIR) + "/tls/snakeoil.cer"; }
std::string key_file() { return std::string(ATHENA_TEST_SOURCE_DIR) + "/tls/snakeoil.key"; }
std::string ca_file() { return std::string(ATHENA_TEST_SOURCE_DIR) + "/tls/ca/snakeca.crt"; }

// A TLS listener on loopback with the snakeoil certificate (127.0.0.1, signed by the snakeoil CA). It says whether a
// client finished the handshake and sent anything after it, which a client that trusts it does at once.
class TlsListener {
 public:
  TlsListener() : _ssl(boost::asio::ssl::context::tls_server), _acceptor(_io) {
    _ssl.use_certificate_chain_file(cert_file());
    _ssl.use_private_key_file(key_file(), boost::asio::ssl::context::pem);

    const boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::make_address("127.0.0.1"), 0);
    _acceptor.open(endpoint.protocol());
    _acceptor.set_option(boost::asio::socket_base::reuse_address(true));
    _acceptor.bind(endpoint);
    _acceptor.listen();
    _port = _acceptor.local_endpoint().port();

    _acceptor.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
      if (ec) return;
      _stream = std::make_unique<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>(std::move(socket), _ssl);
      _stream->async_handshake(boost::asio::ssl::stream_base::server, [this](boost::system::error_code failed) {
        if (failed) return;
        _handshaken = true;
        _stream->async_read_some(boost::asio::buffer(_buffer), [this](boost::system::error_code read_failed, std::size_t length) {
          if (!read_failed && length > 0) _heard = true;
        });
      });
    });
    _thread = std::thread([this]() { _io.run(); });
  }

  ~TlsListener() {
    _io.stop();
    if (_thread.joinable()) _thread.join();
  }

  std::uint16_t port() const { return _port; }
  bool handshaken() const { return _handshaken; }

  // Waits up to `bound` for the client to send something over the finished handshake.
  bool heard_within(std::chrono::milliseconds bound) const {
    const auto until = std::chrono::steady_clock::now() + bound;
    while (!_heard && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return _heard;
  }

 private:
  boost::asio::io_context _io;
  boost::asio::ssl::context _ssl;
  boost::asio::ip::tcp::acceptor _acceptor;
  std::unique_ptr<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> _stream;
  std::array<char, 1024> _buffer{};
  std::uint16_t _port = 0;
  std::atomic<bool> _handshaken{false};
  std::atomic<bool> _heard{false};
  std::thread _thread;
};

// Starts a rediss:// connection to the listener with these settings and lets it run for `bound`.
bool reached(TlsListener& listener, const std::string& settings, std::chrono::milliseconds bound = std::chrono::seconds(2)) {
  auto logger = std::make_shared<MockLogger>();
  auto url = std::make_shared<types::URL>("rediss://:secret@127.0.0.1:" + std::to_string(listener.port()) + "/0");
  auto datastore = std::make_shared<RedisDatastore>(logger, url);
  EXPECT_TRUE(datastore->configure(YAML::Load(settings), Config(logger)));

  boost::asio::io_context io;
  datastore->connect(io.get_executor(), [](plugins::Status) {});
  const bool heard = listener.heard_within(bound);
  datastore->close();
  return heard;
}

}  // namespace

// A server whose certificate nothing trusts is not sent the password, or anything else.
TEST(RedisTlsTest, AServerNobodyTrustsIsToldNothing) {
  TlsListener listener;
  EXPECT_FALSE(reached(listener, "{}"));
  EXPECT_FALSE(listener.handshaken());
}

// redis.ca_file adds a CA to trust, as mqtt.ca_file does for the broker, and the name in the URL is on the certificate.
TEST(RedisTlsTest, AServerSignedByTheConfiguredCaIsSpokenTo) {
  TlsListener listener;
  EXPECT_TRUE(reached(listener, "ca_file: " + ca_file()));
  EXPECT_TRUE(listener.handshaken());
}

// redis.ca_file is one of the driver's settings, so a misspelt key is named at startup.
TEST(RedisTlsTest, TheCaFileIsADocumentedSetting) {
  bool found = false;
  for (const auto& setting : RedisDatastore::settings()) {
    if (setting.key == "redis.ca_file") found = true;
  }
  EXPECT_TRUE(found);
}
