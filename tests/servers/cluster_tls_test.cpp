//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>
#include <unistd.h>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <filesystem>
#include <future>
#include <memory>
#include <string>

#include "../helpers/core_fixture_helper.h"
#include "channel.h"
#include "cluster_ca.h"
#include "servers/tls_server.h"

using namespace athenasip;

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;

namespace {

// A cluster of two, node-a and node-b, and an outsider with a certificate from its own authority.
struct Certificates {
  std::filesystem::path root;
  std::filesystem::path cluster;
  std::filesystem::path outsider;

  Certificates() {
    root = std::filesystem::temp_directory_path() / ("athenasip-cluster-" + std::to_string(::getpid()) + "-" + std::to_string(std::rand()));
    cluster = root / "cluster";
    outsider = root / "outsider";

    ca::init(cluster.string());
    ca::issue_node(cluster.string(), "node-a", {"127.0.0.1"});
    ca::issue_node(cluster.string(), "node-b", {"127.0.0.1"});

    ca::init(outsider.string());
    ca::issue_node(outsider.string(), "node-x", {"127.0.0.1"});
  }

  ~Certificates() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  std::string file(const std::filesystem::path& dir, const std::string& name) const { return (dir / name).string(); }
};

struct ClusterFixture : CoreFixture {
  Certificates certificates;
  std::shared_ptr<servers::TLSServer> server;

  // The inter-node listener: node-a's certificate, and a peer must show one from the cluster CA.
  explicit ClusterFixture(const std::string& server_dir = "cluster", const std::string& server_node = "node-a") {
    seed_realm("example.com");

    const auto& dir = server_dir == "cluster" ? certificates.cluster : certificates.outsider;
    server = std::make_shared<servers::TLSServer>(logger, core, "127.0.0.1", 0);
    EXPECT_TRUE(server->set_certificates(certificates.file(dir, server_node + ".crt"), certificates.file(dir, server_node + ".key")));
    EXPECT_TRUE(server->require_peer_certificates(certificates.file(certificates.cluster, "ca.crt")));
    server->start();
  }

  ~ClusterFixture() {
    on_strand([this]() { core->channel_close_all(); });
    settle();
    server->stop();
  }

  // A client handshake from outside the node: with which certificate, if any.
  boost::system::error_code handshake_as(const std::filesystem::path& dir, const std::string& node) {
    net::io_context io;
    ssl::context context(ssl::context::tls_client);
    context.load_verify_file(certificates.file(certificates.cluster, "ca.crt"));
    context.set_verify_mode(ssl::verify_peer);
    if (!node.empty()) {
      context.use_certificate_chain_file(certificates.file(dir, node + ".crt"));
      context.use_private_key_file(certificates.file(dir, node + ".key"), ssl::context::pem);
    }

    ssl::stream<tcp::socket> stream(io, context);
    stream.lowest_layer().connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), server->port()));

    boost::system::error_code error;
    stream.handshake(ssl::stream_base::client, error);

    // TLS 1.3 sends the client's certificate after the client considers the handshake done, so a refusal arrives
    // on the first read. A member is answered: a REGISTER with no credentials is challenged.
    if (!error) {
      std::string request = "REGISTER sip:example.com SIP/2.0\r\n";
      request += "Via: SIP/2.0/TLS 127.0.0.1:9;branch=z9hG4bK-cluster\r\n";
      request += "From: <sip:alice@example.com>;tag=a\r\nTo: <sip:alice@example.com>\r\n";
      request += "Call-ID: cluster\r\nCSeq: 1 REGISTER\r\nContact: <sip:alice@127.0.0.1:9>\r\nMax-Forwards: 70\r\nContent-Length: 0\r\n\r\n";
      net::write(stream, net::buffer(request), error);

      std::array<char, 2048> buffer{};
      bool answered = false;
      if (!error) {
        stream.async_read_some(net::buffer(buffer), [&error, &answered](const boost::system::error_code& ec, std::size_t) {
          error = ec;
          answered = !ec;
        });
        io.run_for(std::chrono::seconds(2));
        if (!answered && !error) error = net::error::timed_out;
      }
    }
    return error;
  }

  plugins::Result<std::shared_ptr<Channel>> connect(std::uint16_t port) {
    std::promise<plugins::Result<std::shared_ptr<Channel>>> promise;
    auto future = promise.get_future();
    core->post([&]() {
      core->channel_connect("tls", "127.0.0.1", port, [&promise](plugins::Result<std::shared_ptr<Channel>> result) { promise.set_value(std::move(result)); });
    });
    return future.get();
  }
};

}  // namespace

// Mutual TLS: a peer that shows a certificate from the cluster CA is let in.
TEST(ClusterTlsTest, APeerWithAClusterCertificateIsLetIn) {
  ClusterFixture f;
  EXPECT_FALSE(f.handshake_as(f.certificates.cluster, "node-b")) << "node-b is a member of the cluster";
}

// A peer that shows no certificate, or one from another authority, is refused.
TEST(ClusterTlsTest, APeerWithoutOneIsRefused) {
  ClusterFixture f;
  EXPECT_TRUE(f.handshake_as(f.certificates.cluster, "")) << "no certificate";
  EXPECT_TRUE(f.handshake_as(f.certificates.outsider, "node-x")) << "another authority's certificate";
}

// Outbound: this node shows its own certificate and checks the peer's against the cluster CA and the address
// it dialled. The channel says which node is at the other end.
TEST(ClusterTlsTest, ANodeOpensTlsToAPeerAndKnowsWhoItIs) {
  ClusterFixture f;
  f.on_strand([&f]() {
    f.core->cluster_tls_set(f.certificates.file(f.certificates.cluster, "ca.crt"), f.certificates.file(f.certificates.cluster, "node-b.crt"),
                            f.certificates.file(f.certificates.cluster, "node-b.key"));
  });

  const auto opened = f.connect(f.server->port());
  ASSERT_TRUE(opened.ok) << opened.error;
  EXPECT_EQ(opened.value->peer_node(), "node-a");
}

// A server that cannot prove it is a member is refused.
TEST(ClusterTlsTest, ANodeRefusesAPeerFromAnotherAuthority) {
  ClusterFixture f("outsider", "node-x");
  f.on_strand([&f]() {
    f.core->cluster_tls_set(f.certificates.file(f.certificates.cluster, "ca.crt"), f.certificates.file(f.certificates.cluster, "node-b.crt"),
                            f.certificates.file(f.certificates.cluster, "node-b.key"));
  });

  EXPECT_FALSE(f.connect(f.server->port()).ok);
}
