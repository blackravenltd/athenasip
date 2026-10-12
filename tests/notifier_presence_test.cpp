//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "helpers/proxy_fixture_helper.h"

using namespace athenasip;

namespace {

std::string pidf(const std::string& id, const std::string& basic) {
  return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<presence xmlns=\"urn:ietf:params:xml:ns:pidf\" entity=\"sip:alice@example.com\"><tuple id=\"" + id +
         "\"><status><basic>" + basic + "</basic></status></tuple></presence>\r\n";
}

// Carol keeps a buddy list (RFC 3856): she watches Alice, who publishes from her softphone, and Bob, whose phone
// publishes nothing and is registered. Carol is in the same realm and proven by her REGISTER.
struct PresenceFixture : ProxyFixture {
  std::shared_ptr<MockConnection> carol_connection;
  std::shared_ptr<Channel> carol;
  int subscriptions = 0;
  int published = 0;

  PresenceFixture() {
    bind_bob();
    seed_subscriber(3, "sip:carol@example.com", "carol-ha1");
    carol = make_channel("192.0.2.30", &carol_connection);
    on_strand([this]() { carol->authenticated_as("sip:carol@example.com"); });
  }

  void watch(const std::string& presentity, const std::string& accept = "application/pidf+xml") {
    ++subscriptions;
    std::string raw = "SUBSCRIBE " + presentity + " SIP/2.0\r\n";
    raw += "Via: SIP/2.0/TCP 192.0.2.30:5060;branch=z9hG4bK-carol-" + std::to_string(subscriptions) + "\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "From: <sip:carol@example.com>;tag=carol-" + std::to_string(subscriptions) + "\r\n";
    raw += "To: <" + presentity + ">\r\n";
    raw += "Call-ID: buddy-" + std::to_string(subscriptions) + "\r\n";
    raw += "CSeq: 1 SUBSCRIBE\r\n";
    raw += "Contact: <sip:carol@192.0.2.30:5060>\r\n";
    raw += "Event: presence\r\n";
    raw += "Expires: 600\r\n";
    if (!accept.empty()) raw += "Accept: " + accept + "\r\n";
    raw += "\r\n";
    receive(carol, raw);
    answer_notifies();
  }

  // A refresh of Carol's first subscription, to the Contact the node gave her.
  void refresh() {
    auto ok = response_with(carol_connection, 200);
    if (!ok || !ok->header->contains("Contact")) {
      ADD_FAILURE() << "no subscription to refresh";
      return;
    }
    const auto contact = ok->header->headers_map["Contact"][0]->to_string();
    const auto uri = contact.substr(contact.find('<') + 1, contact.find('>') - contact.find('<') - 1);
    const auto to = ok->header->headers_map["To"][0]->to_string();

    std::string raw = "SUBSCRIBE " + uri + " SIP/2.0\r\n";
    raw += "Via: SIP/2.0/TCP 192.0.2.30:5060;branch=z9hG4bK-carol-refresh\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "From: <sip:carol@example.com>;tag=carol-1\r\n";
    raw += "To: " + to + "\r\n";
    raw += "Call-ID: buddy-1\r\n";
    raw += "CSeq: 2 SUBSCRIBE\r\n";
    raw += "Contact: <sip:carol@192.0.2.30:5060>\r\n";
    raw += "Event: presence\r\n";
    raw += "Expires: 600\r\n";
    raw += "\r\n";
    receive(carol, raw);
    answer_notifies();
  }

  // Alice's softphone publishes, as RFC 3903 4.1 and 4.3 have it. Returns the entity tag.
  std::string publish(const std::string& body, const std::string& if_match = "", const std::string& expires = "600") {
    ++published;
    std::string raw = "PUBLISH sip:alice@example.com SIP/2.0\r\n";
    raw += "Via: SIP/2.0/TCP 192.0.2.10:5060;branch=z9hG4bK-pub" + std::to_string(published) + "\r\n";
    raw += "Max-Forwards: 70\r\n";
    raw += "From: <sip:alice@example.com>;tag=publisher\r\n";
    raw += "To: <sip:alice@example.com>\r\n";
    raw += "Call-ID: pub-" + std::to_string(published) + "\r\n";
    raw += "CSeq: 1 PUBLISH\r\n";
    raw += "Event: presence\r\n";
    raw += "Expires: " + expires + "\r\n";
    if (!if_match.empty()) raw += "SIP-If-Match: " + if_match + "\r\n";
    if (!body.empty()) raw += "Content-Type: application/pidf+xml\r\n";
    raw += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    raw += "\r\n";
    raw += body;
    receive(caller, raw);
    answer_notifies();

    std::string etag;
    for (const auto& message : written(caller_connection)) {
      if (message->header->type == SIPHeader::Type::Response && message->header->contains("SIP-ETag") &&
          message->header->headers_map["Call-ID"][0]->to_string() == "pub-" + std::to_string(published)) {
        etag = message->header->headers_map["SIP-ETag"][0]->to_string();
      }
    }
    return etag;
  }

  void advance(std::chrono::seconds by) {
    on_strand([this, by]() { timers->advance(std::chrono::duration_cast<std::chrono::milliseconds>(by)); });
    settle();
    answer_notifies();
  }

  void answer_notifies() {
    settle();
    for (const auto& notify : requests_with(carol_connection, "NOTIFY")) {
      const auto cseq = notify->header->headers_map["CSeq"][0]->to_string();
      const auto key = notify->header->headers_map["Call-ID"][0]->to_string() + cseq;
      if (_answered.count(key)) continue;
      _answered.insert(key);

      std::string raw = "SIP/2.0 200 OK\r\n";
      for (const auto& via : notify->header->headers_map["Via"]) raw += "Via: " + via->to_string() + "\r\n";
      raw += "From: " + notify->header->headers_map["From"][0]->to_string() + "\r\n";
      raw += "To: " + notify->header->headers_map["To"][0]->to_string() + "\r\n";
      raw += "Call-ID: " + notify->header->headers_map["Call-ID"][0]->to_string() + "\r\n";
      raw += "CSeq: " + cseq + "\r\n";
      raw += "\r\n";
      receive(carol, raw);
    }
  }

  std::vector<std::shared_ptr<SIPMessage>> notifies(int subscription = 1) {
    std::vector<std::shared_ptr<SIPMessage>> found;
    for (const auto& notify : requests_with(carol_connection, "NOTIFY")) {
      if (notify->header->headers_map["Call-ID"][0]->to_string() == "buddy-" + std::to_string(subscription)) found.push_back(notify);
    }
    return found;
  }

  std::string latest(int subscription = 1) {
    auto all = notifies(subscription);
    return all.empty() ? "" : all.back()->body;
  }

  // Every value of the header, as one list.
  static std::string value_of(const std::shared_ptr<SIPMessage>& message, const std::string& name) {
    if (!message || !message->header->contains(name)) return "";
    std::string out;
    for (const auto& value : message->header->headers_map[name]) out += (out.empty() ? "" : ", ") + value->to_string();
    return out;
  }

 private:
  std::set<std::string> _answered;
};

}  // namespace

// RFC 3856 6.6, RFC 6665 4.2.1: the node is the notifier for presence, and its first NOTIFY is a PIDF document
// for the presentity.
TEST(PresencePackageTest, ASubscribeForPresenceIsAnsweredWithPidf) {
  PresenceFixture f;
  f.watch("sip:bob@example.com");

  auto ok = f.response_with(f.carol_connection, 200);
  ASSERT_NE(ok, nullptr);
  EXPECT_TRUE(f.requests_with(f.callee_connection, "SUBSCRIBE").empty()) << "Bob's phone is not asked";

  auto all = f.notifies();
  ASSERT_EQ(all.size(), 1u);
  EXPECT_EQ(PresenceFixture::value_of(all[0], "Event"), "presence");
  EXPECT_EQ(PresenceFixture::value_of(all[0], "Content-Type"), "application/pidf+xml");
  EXPECT_NE(all[0]->body.find("<presence xmlns=\"urn:ietf:params:xml:ns:pidf\""), std::string::npos);
  EXPECT_NE(all[0]->body.find("entity=\"sip:bob@example.com\""), std::string::npos);
}

// docs/design/presence.md: with nothing published, a registered subscriber is open and one with no binding closed.
TEST(PresencePackageTest, WithNothingPublishedTheRegistrationsSay) {
  PresenceFixture f;
  f.watch("sip:bob@example.com");
  f.watch("sip:alice@example.com");

  EXPECT_NE(f.latest(1).find("<basic>open</basic>"), std::string::npos) << f.latest(1);
  EXPECT_NE(f.latest(2).find("<basic>closed</basic>"), std::string::npos) << f.latest(2);
  EXPECT_EQ(f.latest(1).find("<contact"), std::string::npos) << "no contact URIs are given out";
}

// The publications are composed, and stand in for the registrations.
TEST(PresencePackageTest, WhatIsPublishedIsWhatIsSeen) {
  PresenceFixture f;
  f.publish(pidf("softphone", "open"));
  f.watch("sip:alice@example.com");

  const auto body = f.latest();
  EXPECT_NE(body.find("<tuple id=\"softphone\">"), std::string::npos) << body;
  EXPECT_EQ(body.find("id=\"registration\""), std::string::npos) << "published state replaces the registrations'";
}

// RFC 3856 6.6.2: no more than one notification every five seconds. A change inside the window is held, and the
// latest state goes when it opens.
TEST(PresencePackageTest, AChangeWithinFiveSecondsWaitsForTheWindow) {
  PresenceFixture f;
  f.watch("sip:alice@example.com");
  ASSERT_EQ(f.notifies().size(), 1u);

  const auto etag = f.publish(pidf("softphone", "open"));
  f.publish(pidf("softphone", "closed"), etag);
  EXPECT_EQ(f.notifies().size(), 1u) << "held";

  f.advance(std::chrono::seconds(5));
  auto all = f.notifies();
  ASSERT_EQ(all.size(), 2u) << "one NOTIFY for both changes";
  EXPECT_NE(all.back()->body.find("<basic>closed</basic>"), std::string::npos) << all.back()->body;
  EXPECT_NE(all.back()->body.find("softphone"), std::string::npos);
}

// Once the window has passed, a change goes at once.
TEST(PresencePackageTest, AChangeAfterTheWindowGoesAtOnce) {
  PresenceFixture f;
  f.watch("sip:alice@example.com");
  f.advance(std::chrono::seconds(6));

  f.publish(pidf("softphone", "open"));
  auto all = f.notifies();
  ASSERT_EQ(all.size(), 2u);
  EXPECT_NE(all.back()->body.find("softphone"), std::string::npos);
}

// RFC 6665 4.2.1.2: every SUBSCRIBE is followed by a NOTIFY, window or not.
TEST(PresencePackageTest, ARefreshIsNotifiedAtOnce) {
  PresenceFixture f;
  f.watch("sip:bob@example.com");
  f.refresh();
  EXPECT_EQ(f.notifies().size(), 2u);
}

// A binding going is a change: Bob unregistering closes him.
TEST(PresencePackageTest, TheLastBindingGoingClosesThePresentity) {
  PresenceFixture f;
  f.watch("sip:bob@example.com");
  f.advance(std::chrono::seconds(6));

  ASSERT_TRUE(f.unregister_binding(f.bob, std::make_shared<types::SIPUri>("sip:bob@192.0.2.20:5060"), f.callee));
  f.answer_notifies();
  EXPECT_NE(f.latest().find("<basic>closed</basic>"), std::string::npos) << f.latest();
}

// And a binding arriving opens one.
TEST(PresencePackageTest, ARegistrationOpensThePresentity) {
  PresenceFixture f;
  f.watch("sip:alice@example.com");
  f.advance(std::chrono::seconds(6));

  ASSERT_TRUE(f.register_binding(f.alice, std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060"), f.caller, 3600));
  f.answer_notifies();
  EXPECT_NE(f.latest().find("<basic>open</basic>"), std::string::npos) << f.latest();
}

// RFC 3903 4.4: a removed publication leaves the registrations to speak.
TEST(PresencePackageTest, RemovingThePublicationFallsBackToTheRegistrations) {
  PresenceFixture f;
  const auto etag = f.publish(pidf("softphone", "open"));
  f.watch("sip:alice@example.com");
  f.advance(std::chrono::seconds(6));

  f.publish("", etag, "0");
  EXPECT_NE(f.latest().find("id=\"registration\""), std::string::npos) << f.latest();
}

// RFC 6665 4.1.2.1 and 8.2: a watcher that cannot take PIDF is refused.
TEST(PresencePackageTest, AnAcceptWithoutPidfIs406) {
  PresenceFixture f;
  f.watch("sip:bob@example.com", "application/xpidf+xml");
  EXPECT_NE(f.response_with(f.carol_connection, 406), nullptr);
  EXPECT_TRUE(f.notifies().empty());
}

// RFC 6665 7.2.2: both packages are listed.
TEST(PresencePackageTest, AllowEventsListsBothPackages) {
  PresenceFixture f;
  f.watch("sip:bob@example.com");
  const auto allow =
      PresenceFixture::value_of(f.response_with(f.carol_connection, 200), "Allow-Events") + "," + PresenceFixture::value_of(f.notifies()[0], "Allow-Events");
  EXPECT_NE(allow.find("presence"), std::string::npos) << allow;
  EXPECT_NE(allow.find("dialog"), std::string::npos) << allow;
}
