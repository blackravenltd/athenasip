//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "events/topics.h"
#include "helpers/proxy_fixture_helper.h"
#include "notifier.h"
#include "types/presence_record.h"

using namespace athenasip;

namespace {

using Kind = types::PresenceRecord::Kind;

constexpr const char* kPidf =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
    "<presence xmlns=\"urn:ietf:params:xml:ns:pidf\" entity=\"sip:alice@example.com\">"
    "<tuple id=\"phone\"><status><basic>open</basic></status></tuple></presence>\r\n";

// Alice publishes her own presence (RFC 3903) from the connection that carried her REGISTER, so she is proven as
// herself. The node is the event state compositor for the subscribers in its realms.
struct PublishFixture : ProxyFixture {
  int published = 0;

  // What the bus carried for presentities, as every node in a cluster would hear it.
  std::mutex mutex;
  std::vector<std::string> changes;
  std::shared_ptr<events::Subscription> listening;

  PublishFixture() {
    bind_bob();
    listening = bus->subscribe(events::topics::presence_changed_filter(), [this](std::string topic, std::string) {
      std::lock_guard<std::mutex> lock(mutex);
      changes.push_back(events::topics::presence_changed_aor(topic));
    });
  }

  std::string publish(const std::string& body = kPidf, const std::string& if_match = "", const std::string& expires = "600",
                      const std::string& event = "presence", const std::string& content_type = "application/pidf+xml",
                      const std::string& to = "sip:alice@example.com") {
    ++published;
    std::string raw = "PUBLISH " + to + " SIP/2.0\r\n";
    raw += "Via: SIP/2.0/TCP 192.0.2.10:5060;branch=z9hG4bK-pub" + std::to_string(published) + "\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "From: <sip:alice@example.com>;tag=publisher\r\n";
    raw += "To: <" + to + ">\r\n";
    raw += "Call-ID: pub-" + std::to_string(published) + "\r\n";
    raw += "CSeq: 1 PUBLISH\r\n";
    if (!event.empty()) raw += "Event: " + event + "\r\n";
    if (!expires.empty()) raw += "Expires: " + expires + "\r\n";
    if (!if_match.empty()) raw += "SIP-If-Match: " + if_match + "\r\n";
    if (!body.empty()) raw += "Content-Type: " + content_type + "\r\n";
    raw += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    raw += "\r\n";
    raw += body;
    return raw;
  }

  // The answer to the latest PUBLISH, whatever its code.
  std::shared_ptr<SIPMessage> answer() {
    std::shared_ptr<SIPMessage> last;
    for (const auto& message : written(caller_connection)) {
      if (message->header->type == SIPHeader::Type::Response && value_of(message, "CSeq").find("PUBLISH") != std::string::npos &&
          value_of(message, "Call-ID") == "pub-" + std::to_string(published)) {
        last = message;
      }
    }
    return last;
  }

  int code() {
    auto latest = answer();
    return latest ? latest->header->response_code : 0;
  }

  std::string etag() { return value_of(answer(), "SIP-ETag"); }

  std::vector<types::PresenceRecord> publications(const std::string& aor = "sip:alice@example.com") { return store->presence_list(aor, Kind::Publication); }

  std::vector<std::string> heard() {
    settle();
    std::lock_guard<std::mutex> lock(mutex);
    return changes;
  }

  static std::string value_of(const std::shared_ptr<SIPMessage>& message, const std::string& name) {
    return message && message->header->contains(name) ? message->header->headers_map[name][0]->to_string() : "";
  }
};

}  // namespace

// RFC 3903 4.1, 6 step 6: an initial publication is answered 200 with an entity tag and the granted expiry, and
// the state is kept, by entity tag, for the presentity.
TEST(PublishTest, AnInitialPublicationIsKeptAndGivenAnEntityTag) {
  PublishFixture f;
  f.receive(f.caller, f.publish());

  ASSERT_EQ(f.code(), 200);
  EXPECT_FALSE(f.etag().empty());
  EXPECT_EQ(PublishFixture::value_of(f.answer(), "Expires"), "600");
  EXPECT_TRUE(f.requests_with(f.caller_connection, "PUBLISH").empty()) << "answered by the node, not forwarded to Alice's devices";

  auto kept = f.publications();
  ASSERT_EQ(kept.size(), 1u);
  EXPECT_EQ(kept[0].id, f.etag());
  EXPECT_EQ(kept[0].content_type, "application/pidf+xml");
  EXPECT_EQ(kept[0].body, kPidf);
  EXPECT_TRUE(kept[0].node_id.empty()) << "a publication is the presentity's, not a node's";
}

// RFC 3903 6 step 6: entity tags are unique; two publications from two devices are two.
TEST(PublishTest, EachPublicationHasItsOwnEntityTag) {
  PublishFixture f;
  f.receive(f.caller, f.publish());
  const auto first = f.etag();
  f.receive(f.caller, f.publish());
  const auto second = f.etag();

  EXPECT_NE(first, second);
  EXPECT_GE(first.size(), 22u) << "128 random bits";
  EXPECT_EQ(f.publications().size(), 2u);
}

// RFC 3903 4.2: a refresh carries the entity tag and no body. It extends the publication under a new tag, and the
// old one is no longer valid.
TEST(PublishTest, ARefreshKeepsTheStateUnderANewEntityTag) {
  PublishFixture f;
  f.receive(f.caller, f.publish());
  const auto first = f.etag();

  f.receive(f.caller, f.publish("", first, "900"));
  ASSERT_EQ(f.code(), 200);
  const auto second = f.etag();
  EXPECT_FALSE(second.empty());
  EXPECT_NE(second, first) << "RFC 3903 6 step 6: a new entity tag on every success";
  EXPECT_EQ(PublishFixture::value_of(f.answer(), "Expires"), "900");

  auto kept = f.publications();
  ASSERT_EQ(kept.size(), 1u);
  EXPECT_EQ(kept[0].id, second);
  EXPECT_EQ(kept[0].body, kPidf) << "a refresh leaves the state as it was";

  f.receive(f.caller, f.publish("", first));
  EXPECT_EQ(f.code(), 412) << "the old tag is spent";
}

// RFC 3903 4.3: a modification carries the entity tag and a body, which replaces the state.
TEST(PublishTest, AModificationReplacesTheState) {
  PublishFixture f;
  f.receive(f.caller, f.publish());
  const auto first = f.etag();

  std::string closed = kPidf;
  closed.replace(closed.find("open"), 4, "closed");
  f.receive(f.caller, f.publish(closed, first));
  ASSERT_EQ(f.code(), 200);
  EXPECT_NE(f.etag(), first);

  auto kept = f.publications();
  ASSERT_EQ(kept.size(), 1u);
  EXPECT_EQ(kept[0].id, f.etag());
  EXPECT_EQ(kept[0].body, closed);
}

// RFC 3903 4.4: Expires 0 with the entity tag removes the publication.
TEST(PublishTest, ExpiresZeroRemovesThePublication) {
  PublishFixture f;
  f.receive(f.caller, f.publish());
  const auto first = f.etag();

  f.receive(f.caller, f.publish("", first, "0"));
  ASSERT_EQ(f.code(), 200);
  EXPECT_EQ(PublishFixture::value_of(f.answer(), "Expires"), "0");
  EXPECT_TRUE(f.publications().empty());
}

// RFC 3903 6 step 4: an entity tag the compositor does not hold is 412.
TEST(PublishTest, AnUnknownEntityTagIs412) {
  PublishFixture f;
  f.receive(f.caller, f.publish("", "no-such-tag"));
  EXPECT_EQ(f.code(), 412);

  f.receive(f.caller, f.publish(kPidf, "no-such-tag"));
  EXPECT_EQ(f.code(), 412) << "a modification of nothing too";
  EXPECT_TRUE(f.publications().empty());
}

// RFC 3903 6 step 4: an initial publication must carry state.
TEST(PublishTest, NoBodyAndNoEntityTagIs400) {
  PublishFixture f;
  f.receive(f.caller, f.publish(""));
  EXPECT_EQ(f.code(), 400);
  EXPECT_TRUE(f.publications().empty());
}

// RFC 3903 6 step 5, RFC 3856 6.1: the presence package's state is PIDF.
TEST(PublishTest, ABodyThatIsNotPidfIs415) {
  PublishFixture f;
  f.receive(f.caller, f.publish("hello", "", "600", "presence", "text/plain"));
  ASSERT_EQ(f.code(), 415);
  EXPECT_NE(PublishFixture::value_of(f.answer(), "Accept").find("application/pidf+xml"), std::string::npos);
  EXPECT_TRUE(f.publications().empty());
}

// RFC 3903 6 step 3: under the minimum is 423 with Min-Expires; over the maximum is granted the maximum.
TEST(PublishTest, TheExpiryIsBoundedLikeASubscriptions) {
  PublishFixture f;
  f.receive(f.caller, f.publish(kPidf, "", "5"));
  ASSERT_EQ(f.code(), 423);
  EXPECT_EQ(PublishFixture::value_of(f.answer(), "Min-Expires"), std::to_string(f.config->presence_min_expires));

  f.receive(f.caller, f.publish(kPidf, "", "86400"));
  ASSERT_EQ(f.code(), 200);
  EXPECT_EQ(PublishFixture::value_of(f.answer(), "Expires"), std::to_string(f.config->presence_max_expires));

  f.receive(f.caller, f.publish(kPidf, "", ""));
  ASSERT_EQ(f.code(), 200);
  EXPECT_EQ(PublishFixture::value_of(f.answer(), "Expires"), "3600") << "the default when none is asked for";
}

// RFC 3903 6 step 2: only the presence package is published here.
TEST(PublishTest, AnotherPackageIs489) {
  PublishFixture f;
  f.receive(f.caller, f.publish(kPidf, "", "600", "dialog"));
  ASSERT_EQ(f.code(), 489);
  EXPECT_EQ(PublishFixture::value_of(f.answer(), "Allow-Events"), "presence");

  f.receive(f.caller, f.publish(kPidf, "", "600", ""));
  EXPECT_EQ(f.code(), 489) << "no Event header names no package served";
  EXPECT_TRUE(f.publications().empty());
}

// docs/design/presence.md: only the presentity publishes its own state.
TEST(PublishTest, PublishingForSomeoneElseIs403) {
  PublishFixture f;
  f.receive(f.caller, f.publish(kPidf, "", "600", "presence", "application/pidf+xml", "sip:bob@example.com"));
  EXPECT_EQ(f.code(), 403);
  EXPECT_TRUE(f.publications("sip:bob@example.com").empty());
}

TEST(PublishTest, PublishingForNobodyIs404) {
  PublishFixture f;
  f.receive(f.caller, f.publish(kPidf, "", "600", "presence", "application/pidf+xml", "sip:nobody@example.com"));
  EXPECT_EQ(f.code(), 404);
}

// A change of state is signalled on the bus once it is stored; a refresh changes nothing.
TEST(PublishTest, AChangeOfStateIsSignalledAndARefreshIsNot) {
  PublishFixture f;
  f.receive(f.caller, f.publish());
  EXPECT_EQ(f.heard(), (std::vector<std::string>{"sip:alice@example.com"}));

  f.receive(f.caller, f.publish("", f.etag()));
  EXPECT_EQ(f.heard().size(), 1u) << "a refresh";

  f.receive(f.caller, f.publish("", f.etag(), "0"));
  EXPECT_EQ(f.heard().size(), 2u) << "a removal";
}

// With presence off, PUBLISH is proxied as before.
TEST(PublishTest, WithPresenceOffAPublishIsForwarded) {
  PublishFixture f;
  f.config->presence_enable = false;
  f.receive(f.caller, f.publish(kPidf, "", "600", "presence", "application/pidf+xml", "sip:bob@example.com"));

  EXPECT_FALSE(f.requests_with(f.callee_connection, "PUBLISH").empty());
  EXPECT_TRUE(f.publications("sip:bob@example.com").empty());
}

// RFC 3261 11.2: the node says it takes PUBLISH.
TEST(PublishTest, OptionsAllowsPublish) {
  PublishFixture f;
  std::string raw = "OPTIONS sip:example.com SIP/2.0\r\n";
  raw += "Via: SIP/2.0/TCP 192.0.2.10:5060;branch=z9hG4bK-options\r\n";
  raw += "Max-Forwards: 70\r\n";
  raw += "From: <sip:alice@example.com>;tag=options\r\n";
  raw += "To: <sip:example.com>\r\n";
  raw += "Call-ID: options-1\r\n";
  raw += "CSeq: 1 OPTIONS\r\n";
  raw += "\r\n";
  f.receive(f.caller, raw);

  auto ok = f.response_with(f.caller_connection, 200);
  ASSERT_NE(ok, nullptr);
  std::string allow;
  for (const auto& value : ok->header->headers_map["Allow"]) allow += value->to_string() + ",";
  EXPECT_NE(allow.find("PUBLISH"), std::string::npos) << allow;
}
