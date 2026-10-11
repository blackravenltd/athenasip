//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "api/presence_api.h"

#include <gtest/gtest.h>

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <map>
#include <memory>
#include <string>

#include "../helpers/proxy_fixture_helper.h"
#include "../helpers/signed_in_users_helper.h"
#include "api/admin_api.h"
#include "api/router.h"
#include "call.h"
#include "types/presence_record.h"

using namespace athenasip;

namespace {

namespace beast = boost::beast;
namespace http = boost::beast::http;

using Kind = types::PresenceRecord::Kind;

// What the console reads of presence (docs/design/presence.md, The API and the console): who is watching whom, read
// from the datastore, and what each subscriber's state is. Bob is registered here; Carol watches him.
struct PresenceApiFixture : ProxyFixture {
  std::shared_ptr<api::AdminAPI> admin;
  std::shared_ptr<api::Router> router;
  SignedInUsers signed_in;
  std::shared_ptr<api::Sessions> sessions;
  std::shared_ptr<api::PresenceAPI> presence;

  std::shared_ptr<MockConnection> carol_connection;
  std::shared_ptr<Channel> carol;

  struct Response {
    unsigned status = 0;
    boost::json::value json;
  };

  PresenceApiFixture() {
    bind_bob();
    seed_subscriber(3, "sip:carol@example.com", "carol-ha1");
    carol = make_channel("192.0.2.30", &carol_connection, "tcp");
    on_strand([this]() { carol->authenticated_as("sip:carol@example.com"); });

    admin = std::make_shared<api::AdminAPI>(logger, "127.0.0.1", 0);
    auto bearer = std::make_shared<api::BearerAuth>();
    sessions = std::make_shared<api::Sessions>(logger, datastore, admin->executor(), api::Sessions::Lifetimes{3600, 600});
    bearer->sessions_register(sessions);
    router = std::make_shared<api::Router>(bearer);
    presence = std::make_shared<api::PresenceAPI>(logger, core, admin->executor());
    presence->register_routes(*router);
    admin->middlewares.push_back(router->middleware("/api/"));
    admin->start();
    signed_in = SignedInUsers(sessions, *store);
  }

  ~PresenceApiFixture() { admin->stop(); }

  void watch_bob(const std::string& package) {
    std::string raw = "SUBSCRIBE sip:bob@example.com SIP/2.0\r\n";
    raw += "Via: SIP/2.0/TCP 192.0.2.30:5060;branch=z9hG4bK-carol-" + package + "\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "From: <sip:carol@example.com>;tag=carol-" + package + "\r\n";
    raw += "To: <sip:bob@example.com>\r\n";
    raw += "Call-ID: watch-" + package + "\r\n";
    raw += "CSeq: 1 SUBSCRIBE\r\n";
    raw += "Contact: <sip:carol@192.0.2.30:5060>\r\n";
    raw += "Event: " + package + "\r\n";
    raw += "Expires: 600\r\n";
    raw += "\r\n";
    receive(carol, raw);
  }

  Response get(const std::string& target, const std::string& token = "admin-token") {
    boost::asio::io_context io_context;
    boost::asio::ip::tcp::socket socket(io_context);
    socket.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), admin->port()));

    http::request<http::string_body> request{http::verb::get, target, 11};
    request.set(http::field::host, "127.0.0.1");
    if (!token.empty()) request.set(http::field::authorization, "Bearer " + signed_in.presented(token));
    request.prepare_payload();
    http::write(socket, request);

    beast::flat_buffer buffer;
    http::response<http::string_body> response;
    boost::system::error_code ec;
    http::read(socket, buffer, response, ec);
    socket.close(ec);

    boost::system::error_code parse_ec;
    auto parsed = boost::json::parse(response.body(), parse_ec);
    return Response{response.result_int(), parse_ec ? boost::json::value() : parsed};
  }

  static std::string text(const boost::json::value& value, const char* key) { return std::string(value.as_object().at(key).as_string().c_str()); }
};

constexpr const char* kAway =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<presence xmlns=\"urn:ietf:params:xml:ns:pidf\" entity=\"sip:bob@example.com\">"
    "<tuple id=\"desk\"><status><basic>closed</basic></status></tuple>"
    "<note>At lunch</note>"
    "</presence>";

}  // namespace

// Every subscription in the cluster, read from the datastore: this node's, and those another node holds.
TEST(PresenceApiTest, ListsEverySubscriptionInTheCluster) {
  PresenceApiFixture f;
  f.watch_bob("dialog");

  // The same watch, as though node-b held it.
  f.core->nodes()->observe("nodes/node-b/status", "{\"status\":\"ok\",\"node\":\"node-b\"}");
  auto records = f.store->presence_list("sip:bob@example.com", Kind::Subscription);
  ASSERT_EQ(records.size(), 1u);
  auto elsewhere = records[0];
  elsewhere.id = "held-by-b";
  elsewhere.node_id = "node-b";
  ASSERT_TRUE(f.store->presence_put(elsewhere, 600));

  const auto response = f.get("/api/v1/presence/subscriptions");
  ASSERT_EQ(response.status, 200u);
  const auto& listed = response.json.as_array();
  ASSERT_EQ(listed.size(), 2u);

  std::map<std::string, boost::json::object> by_node;
  for (const auto& entry : listed) by_node[PresenceApiFixture::text(entry, "node_id")] = entry.as_object();
  ASSERT_TRUE(by_node.contains("test-node"));
  ASSERT_TRUE(by_node.contains("node-b"));

  const auto& here = by_node["test-node"];
  EXPECT_EQ(here.at("watcher").as_string(), "sip:carol@example.com");
  EXPECT_EQ(here.at("presentity").as_string(), "sip:bob@example.com");
  EXPECT_EQ(here.at("package").as_string(), "dialog");
  EXPECT_EQ(here.at("id").as_string(), records[0].id);
  EXPECT_GT(here.at("expires_at").to_number<std::int64_t>(), 0);
  EXPECT_EQ(by_node["node-b"].at("id").as_string(), "held-by-b");
}

// Who watches whom is the cluster's status, as /nodes and /registrations are.
TEST(PresenceApiTest, TheSubscriptionsNeedTheClusterStatusRole) {
  PresenceApiFixture f;
  EXPECT_EQ(f.get("/api/v1/presence/subscriptions", "client-token").status, 200u);
  EXPECT_EQ(f.get("/api/v1/presence/subscriptions", "scopeless-token").status, 403u);
  EXPECT_EQ(f.get("/api/v1/presence/subscriptions", "").status, 401u);
}

// Nothing published: the bindings speak, as the NOTIFY would (RFC 3856 6.6.1 leaves the composition to the server).
TEST(PresenceApiTest, ASubscriberWhoPublishedNothingIsWhatItsRegistrationsSay) {
  PresenceApiFixture f;

  const auto bob = f.get("/api/v1/realms/example.com/subscribers/bob/presence");
  ASSERT_EQ(bob.status, 200u);
  EXPECT_EQ(PresenceApiFixture::text(bob.json, "subscriber"), "sip:bob@example.com");
  EXPECT_EQ(PresenceApiFixture::text(bob.json, "basic"), "open");
  EXPECT_EQ(PresenceApiFixture::text(bob.json, "source"), "registrations");
  EXPECT_TRUE(bob.json.as_object().at("publications").as_array().empty());
  EXPECT_TRUE(bob.json.as_object().at("dialogs").as_array().empty());

  // Carol has no binding.
  const auto carol = f.get("/api/v1/realms/example.com/subscribers/carol/presence");
  ASSERT_EQ(carol.status, 200u);
  EXPECT_EQ(PresenceApiFixture::text(carol.json, "basic"), "closed");
}

// RFC 3903: what was published is the state, each publication with its entity tag and expiry.
TEST(PresenceApiTest, ASubscriberWhoPublishedIsWhatItPublished) {
  PresenceApiFixture f;
  types::PresenceRecord publication;
  publication.kind = Kind::Publication;
  publication.id = "etag-desk";
  publication.aor = "sip:bob@example.com";
  publication.content_type = "application/pidf+xml";
  publication.body = kAway;
  ASSERT_TRUE(f.store->presence_put(publication, 600));

  const auto bob = f.get("/api/v1/realms/example.com/subscribers/bob/presence");
  ASSERT_EQ(bob.status, 200u);
  EXPECT_EQ(PresenceApiFixture::text(bob.json, "basic"), "closed");
  EXPECT_EQ(PresenceApiFixture::text(bob.json, "source"), "published");

  const auto& publications = bob.json.as_object().at("publications").as_array();
  ASSERT_EQ(publications.size(), 1u);
  EXPECT_EQ(PresenceApiFixture::text(publications[0], "etag"), "etag-desk");
  EXPECT_EQ(PresenceApiFixture::text(publications[0], "content_type"), "application/pidf+xml");
  EXPECT_NE(PresenceApiFixture::text(publications[0], "document").find("At lunch"), std::string::npos);
  EXPECT_GT(publications[0].as_object().at("expires_at").to_number<std::int64_t>(), 0);

  // The composed document, as a watcher is sent it.
  EXPECT_NE(PresenceApiFixture::text(bob.json, "document").find("<tuple id=\"desk\">"), std::string::npos);
}

// The subscriber's dialogs, as RFC 4235 has them from its side: what a BLF key shows.
TEST(PresenceApiTest, ASubscribersDialogsAreItsOpenCalls) {
  PresenceApiFixture f;
  auto call = std::make_shared<Call>();
  call->id = "call-1";
  call->state = Call::State::Connected;
  auto& alice = call->add_participant(std::make_shared<types::SIPIdentity>("sip:alice@example.com"), nullptr, true);
  alice.subscriber = "sip:alice@example.com";
  auto& bob = call->add_participant(std::make_shared<types::SIPIdentity>("sip:bob@example.com"));
  bob.subscriber = "sip:bob@example.com";
  ASSERT_TRUE(f.store->call_create(call));

  const auto response = f.get("/api/v1/realms/example.com/subscribers/bob/presence");
  ASSERT_EQ(response.status, 200u);
  const auto& dialogs = response.json.as_object().at("dialogs").as_array();
  ASSERT_EQ(dialogs.size(), 1u);
  EXPECT_EQ(PresenceApiFixture::text(dialogs[0], "call_id"), "call-1");
  EXPECT_EQ(PresenceApiFixture::text(dialogs[0], "state"), "confirmed");
  EXPECT_EQ(PresenceApiFixture::text(dialogs[0], "direction"), "recipient");
  EXPECT_EQ(PresenceApiFixture::text(dialogs[0], "remote"), "sip:alice@example.com");
}

// A subscriber's state is the realm's to see, as the subscriber is.
TEST(PresenceApiTest, ASubscribersStateNeedsTheSubscribersRole) {
  PresenceApiFixture f;
  EXPECT_EQ(f.get("/api/v1/realms/example.com/subscribers/bob/presence", "client-token").status, 403u);
  EXPECT_EQ(f.get("/api/v1/realms/example.com/subscribers/bob/presence", "").status, 401u);
}

TEST(PresenceApiTest, NoSuchSubscriberIs404) {
  PresenceApiFixture f;
  const auto response = f.get("/api/v1/realms/example.com/subscribers/nobody/presence");
  EXPECT_EQ(response.status, 404u);
  EXPECT_EQ(response.json.as_object().at("error").as_object().at("code").as_string(), "not_found");
}

// With presence off the node is no notifier, and says so rather than inventing a state.
TEST(PresenceApiTest, WithPresenceOffAStateIs404AndSaysWhy) {
  PresenceApiFixture f;
  f.config->presence_enable = false;
  const auto response = f.get("/api/v1/realms/example.com/subscribers/bob/presence");
  EXPECT_EQ(response.status, 404u);
  EXPECT_EQ(response.json.as_object().at("error").as_object().at("code").as_string(), "presence_off");
}
