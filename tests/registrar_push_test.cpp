//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "headers/uint_header.h"
#include "helpers/core_fixture_helper.h"
#include "helpers/fake_push_service_helper.h"
#include "util.h"

using namespace athenasip;
using athenasip::headers::UIntHeader;

namespace {

const std::string kHa1 = Util::md5("alice:example.com:secret");

// The RFC 8599 figure 2 contact, on this fixture's address.
const std::string kPushContact = "<sip:alice@192.0.2.10:5060;pn-provider=acme;pn-param=acme-param;pn-prid=ZTY4ZDJlMzODE1NmUgKi0K>";

struct Fixture : CoreFixture {
  std::shared_ptr<MockConnection> connection;
  std::shared_ptr<Channel> channel;
  std::shared_ptr<FakePushService> acme = std::make_shared<FakePushService>("acme");

  Fixture() {
    seed_realm("example.com");
    seed_subscriber(7, "sip:alice@example.com", kHa1);
    channel = make_channel("192.0.2.10", &connection);
    on_strand([this]() { core->push_register(acme); });
  }

  // An authenticated REGISTER, answered.
  std::shared_ptr<SIPMessage> register_with(const std::string& contact, const std::string& expires = "7200", const std::string& extra = "") {
    connection->written.clear();

    const auto nonce = mint_nonce(store->realm_get_by_name("example.com"));
    const std::string uri = "sip:example.com";
    const auto response = Util::md5(kHa1 + ":" + nonce + ":" + Util::md5("REGISTER:" + uri));

    std::string raw = "REGISTER " + uri + " SIP/2.0\r\n";
    raw += "Via: SIP/2.0/UDP 192.0.2.10:5060;branch=z9hG4bK-reg-" + nonce.substr(0, 8) + "\r\n";
    raw += "From: <sip:alice@example.com>;tag=alice\r\n";
    raw += "To: <sip:alice@example.com>\r\n";
    raw += "Call-ID: call-registrar-push\r\n";
    raw += "CSeq: 1 REGISTER\r\n";
    raw += "Contact: " + contact + "\r\n";
    raw += "Expires: " + expires + "\r\n";
    raw += extra;
    raw += "Authorization: Digest username=\"alice\", realm=\"example.com\", nonce=\"" + nonce + "\", uri=\"" + uri + "\", response=\"" + response + "\"\r\n";
    raw += "\r\n";

    receive(channel, raw);

    const auto messages = written(connection);
    return messages.empty() ? nullptr : messages.back();
  }

  static std::vector<std::string> feature_caps(const std::shared_ptr<SIPMessage>& response) {
    std::vector<std::string> values;
    if (!response || !response->header->contains("Feature-Caps")) return values;
    for (const auto& value : response->header->headers_map["Feature-Caps"]) values.push_back(value->to_string());
    return values;
  }

  bool bound_with_push() {
    for (const auto& binding : store->location_list(7)) {
      if (binding.push) return true;
    }
    return false;
  }
};

}  // namespace

// RFC 8599 4.1.1 and 5.6.1.1: a request for push to a supported service is answered with a Feature-Caps naming
// it, and the binding is one this node will push to.
TEST(RegistrarPushTest, PushToASupportedServiceIsGrantedInFeatureCaps) {
  Fixture f;

  auto response = f.register_with(kPushContact);

  ASSERT_NE(response, nullptr);
  ASSERT_EQ(response->header->response_code, 200);
  EXPECT_EQ(Fixture::feature_caps(response), std::vector<std::string>{"*;+sip.pns=\"acme\""});
  EXPECT_TRUE(f.bound_with_push());
}

// RFC 8599 5.4: a service's other indicators, such as +sip.vapid, share its Feature-Caps.
TEST(RegistrarPushTest, AServicesOtherIndicatorsShareItsFeatureCaps) {
  Fixture f;
  auto vapid = std::make_shared<FakePushService>("webpush", std::vector<std::pair<std::string, std::string>>{{"+sip.vapid", "BKey"}});
  f.on_strand([&]() { f.core->push_register(vapid); });

  auto response = f.register_with("<sip:alice@192.0.2.10:5060;pn-provider=webpush;pn-prid=https%3A%2F%2Fpush.example.org%2Fs%2F1>");

  ASSERT_NE(response, nullptr);
  EXPECT_EQ(Fixture::feature_caps(response), std::vector<std::string>{"*;+sip.pns=\"webpush\";+sip.vapid=\"BKey\""});
}

// RFC 8599 4.1.4 and 5.6.1.1: a client that can refresh on its own timer is told, in +sip.pnsreg, how long
// before expiry it must; the value is over 120 seconds.
TEST(RegistrarPushTest, AClientThatRefreshesItselfIsToldWhenTo) {
  Fixture f;

  auto response = f.register_with(kPushContact + ";+sip.pnsreg");

  ASSERT_NE(response, nullptr);
  const auto caps = Fixture::feature_caps(response);
  ASSERT_EQ(caps.size(), 1u);
  EXPECT_EQ(caps[0], "*;+sip.pns=\"acme\";+sip.pnsreg=\"" + std::to_string(f.config->push_refresh) + "\"");
  EXPECT_GT(f.config->push_refresh, 120u);
}

// RFC 8599 5.6.1.1: a registrar that knows no other proxy serves the service may answer 555.
TEST(RegistrarPushTest, PushToAServiceThisNodeDoesNotServeIs555) {
  Fixture f;

  auto response = f.register_with("<sip:alice@192.0.2.10:5060;pn-provider=other;pn-prid=token>");

  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->header->response_code, 555);
  EXPECT_TRUE(f.store->location_list(7).empty());
}

// RFC 8599 5.6.1.1: so may a request that lacks what the service needs (a pn-param, for this one).
TEST(RegistrarPushTest, PushWithoutWhatTheServiceNeedsIs555) {
  Fixture f;
  f.acme->needs_param = true;

  auto response = f.register_with("<sip:alice@192.0.2.10:5060;pn-provider=acme;pn-prid=token>");

  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->header->response_code, 555);
}

// RFC 8599 5.6.1.1: a binding that would expire before its refresh push is too brief, and 423 says what is
// long enough (RFC 3261 10.3 step 7).
TEST(RegistrarPushTest, ABindingTooBriefToBeWokenInTimeIs423) {
  Fixture f;

  auto response = f.register_with(kPushContact, "60");

  ASSERT_NE(response, nullptr);
  ASSERT_EQ(response->header->response_code, 423);
  auto minimum = response->header->headers_map["Min-Expires"][0]->as<UIntHeader>();
  ASSERT_NE(minimum, nullptr);
  EXPECT_EQ(minimum->value, f.config->push_minimum_expiry());
}

// RFC 8599 5.6.1.1: when the registrar grants less than push needs, the binding is registered without push and
// the 2xx does not claim it.
TEST(RegistrarPushTest, ABindingGrantedTooLittleTimeRegistersWithoutPush) {
  Fixture f;
  auto realm = f.store->realm_get_by_name("example.com");
  realm->registration_timeout = 120;
  f.store->realm_update(realm);

  auto response = f.register_with(kPushContact);

  ASSERT_NE(response, nullptr);
  ASSERT_EQ(response->header->response_code, 200);
  EXPECT_TRUE(Fixture::feature_caps(response).empty());
  EXPECT_FALSE(f.bound_with_push());
}

// RFC 8599 4.1.5 and 5.6.1.2: a pn-provider with no pn-prid asks whether the service is supported. It is
// answered, and nothing is pushed to the binding.
TEST(RegistrarPushTest, AQueryForASupportedServiceIsAnsweredWithoutPush) {
  Fixture f;

  auto response = f.register_with("<sip:alice@192.0.2.10:5060;pn-provider=acme>");

  ASSERT_NE(response, nullptr);
  ASSERT_EQ(response->header->response_code, 200);
  EXPECT_EQ(Fixture::feature_caps(response), std::vector<std::string>{"*;+sip.pns=\"acme\""});
  EXPECT_FALSE(f.bound_with_push());
}

// RFC 8599 5.6.1.2: an empty pn-provider is answered with every supported service, one Feature-Caps each.
TEST(RegistrarPushTest, AQueryNamingNoServiceIsAnsweredWithEveryOne) {
  Fixture f;
  f.on_strand([&]() { f.core->push_register(std::make_shared<FakePushService>("other")); });

  auto response = f.register_with("<sip:alice@192.0.2.10:5060;pn-provider>");

  ASSERT_NE(response, nullptr);
  EXPECT_EQ(Fixture::feature_caps(response), (std::vector<std::string>{"*;+sip.pns=\"acme\"", "*;+sip.pns=\"other\""}));
}

// RFC 8599 4.1.5: 555 to a query means the service is not supported.
TEST(RegistrarPushTest, AQueryForAServiceThisNodeDoesNotServeIs555) {
  Fixture f;

  auto response = f.register_with("<sip:alice@192.0.2.10:5060;pn-provider=other>");

  ASSERT_NE(response, nullptr);
  EXPECT_EQ(response->header->response_code, 555);
}

// RFC 8599 5.6.1.1: a Feature-Caps with +sip.pns in the REGISTER means a proxy nearer the client pushes, so this
// node registers the binding as usual and neither pushes nor refuses.
TEST(RegistrarPushTest, PushOfferedByAnotherProxyIsLeftToIt) {
  Fixture f;

  auto response = f.register_with("<sip:alice@192.0.2.10:5060;pn-provider=other;pn-prid=token>", "7200", "Feature-Caps: *;+sip.pns=\"other\"\r\n");

  ASSERT_NE(response, nullptr);
  ASSERT_EQ(response->header->response_code, 200);
  EXPECT_TRUE(Fixture::feature_caps(response).empty());
  EXPECT_FALSE(f.bound_with_push());
}

// A contact with no pn-* parameters is an ordinary registration, push services or none.
TEST(RegistrarPushTest, AnOrdinaryContactCarriesNoFeatureCaps) {
  Fixture f;

  auto response = f.register_with("<sip:alice@192.0.2.10:5060>");

  ASSERT_NE(response, nullptr);
  ASSERT_EQ(response->header->response_code, 200);
  EXPECT_TRUE(Fixture::feature_caps(response).empty());
}

// RFC 8599 5.5: the client is pushed push.refresh seconds before its binding expires, so it refreshes in time.
TEST(RegistrarPushTest, ABindingIsPushedToRefreshBeforeItExpires) {
  Fixture f;
  ASSERT_EQ(f.register_with(kPushContact, "600")->header->response_code, 200);

  f.timers->advance(std::chrono::seconds(600 - f.config->push_refresh - 1));
  f.settle();
  EXPECT_TRUE(f.acme->sent().empty());

  f.timers->advance(std::chrono::seconds(1));
  f.settle();

  const auto sent = f.acme->sent();
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].reason, push::Notification::Reason::Refresh);
  EXPECT_EQ(sent[0].prid, "ZTY4ZDJlMzODE1NmUgKi0K");
  EXPECT_EQ(sent[0].param, "acme-param");
}

// RFC 8599 5.5: a binding that has been removed is not pushed again.
TEST(RegistrarPushTest, ARemovedBindingIsNotPushedToRefresh) {
  Fixture f;
  ASSERT_EQ(f.register_with(kPushContact, "600")->header->response_code, 200);

  // 4.1.2: the removal carries no pn-prid.
  ASSERT_EQ(f.register_with("<sip:alice@192.0.2.10:5060;pn-provider=acme;pn-param=acme-param>", "0")->header->response_code, 200);

  f.timers->advance(std::chrono::seconds(600));
  f.settle();

  EXPECT_TRUE(f.acme->sent().empty());
}
