//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>

#include "config.h"

#include "mocks/logger_mock.h"

using namespace athenasip;

namespace {

// A configuration file of its own per test, removed with the fixture. Config reads a
// path rather than a string, which is the same thing the node does at startup.
struct ConfigFile {
  std::shared_ptr<MockLogger> logger = std::make_shared<MockLogger>();
  std::filesystem::path path;

  explicit ConfigFile(const std::string& yaml) {
    path = std::filesystem::temp_directory_path() / ("athenasip-config-test-" + std::to_string(::getpid()) + "-" + std::to_string(_next()) + ".yaml");

    std::ofstream out(path);
    out << yaml;
  }

  ~ConfigFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }

  std::shared_ptr<Config> load(bool& ok) {
    auto config = std::make_shared<Config>(logger);
    ok = config->load_from_yaml(path.string());
    return config;
  }

 private:
  static int _next() {
    static int counter = 0;
    return ++counter;
  }
};

}  // namespace

// RFC 7118 over TLS. A browser will not open an insecure WebSocket from a page served
// over https, so this is what says a web client can reach the node at all.
TEST(ConfigTest, ReadsASecureWebsocketListener) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "websocket:\n"
      "  enable: true\n"
      "  address: 127.0.0.1\n"
      "  port: 9501\n"
      "  tls: true\n"
      "  cert_pem_filename: \"../tls/snakeoil.cer\"\n"
      "  key_pem_filename: \"../tls/snakeoil.key\"\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_TRUE(config->websocket_enable);
  EXPECT_TRUE(config->websocket_tls);
  EXPECT_EQ(config->websocket_port, 9501);
  EXPECT_EQ(config->websocket_cert_pem_filename, "../tls/snakeoil.cer");
  EXPECT_EQ(config->websocket_key_pem_filename, "../tls/snakeoil.key");
}

// ws:// is what the listener is without the section saying otherwise, and it stays for
// local development. Nothing about the plain listener changed when the secure one
// arrived.
TEST(ConfigTest, AWebsocketListenerIsPlainUnlessItSaysOtherwise) {
  ConfigFile file(
      "sip:\n  node_id: test-node\n"
      "websocket:\n"
      "  enable: true\n"
      "  address: 0.0.0.0\n"
      "  port: 9500\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_TRUE(config->websocket_enable);
  EXPECT_FALSE(config->websocket_tls);
}

// A listener asked to be secure with nothing to be secure with must not start. Falling
// back to ws:// would be a node quietly serving a browser in the clear, which is the
// failure nobody notices.
TEST(ConfigTest, ASecureWebsocketListenerWithoutCertificatesIsRefused) {
  ConfigFile without_key(
      "sip:\n  node_id: test-node\n"
      "websocket:\n"
      "  enable: true\n"
      "  port: 9501\n"
      "  tls: true\n"
      "  cert_pem_filename: \"../tls/snakeoil.cer\"\n");

  bool ok = true;
  without_key.load(ok);
  EXPECT_FALSE(ok);

  ConfigFile without_cert(
      "sip:\n  node_id: test-node\n"
      "websocket:\n"
      "  enable: true\n"
      "  port: 9501\n"
      "  tls: true\n"
      "  key_pem_filename: \"../tls/snakeoil.key\"\n");

  ok = true;
  without_cert.load(ok);
  EXPECT_FALSE(ok);

  ConfigFile with_neither(
      "sip:\n  node_id: test-node\n"
      "websocket:\n"
      "  enable: true\n"
      "  port: 9501\n"
      "  tls: true\n");

  ok = true;
  with_neither.load(ok);
  EXPECT_FALSE(ok);
}

// The node ships defaulting to what needs no external service, which is the whole of the
// "easy to install" principle in one line: a config that says nothing has to start.
TEST(ConfigTest, DefaultsNeedNoExternalService) {
  ConfigFile file("sip:\n  node_id: test-node\n");

  bool ok = false;
  auto config = file.load(ok);

  ASSERT_TRUE(ok);
  EXPECT_EQ(config->db_url, "memory://");
  EXPECT_EQ(config->media_url, "builtin://");
}
