//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "types/trunk.h"

#include <gtest/gtest.h>

using namespace athenasip::types;

// A trunk is named in scripts and URLs, so its name is a plain word.
TEST(TrunkTest, ANameIsLettersDigitsDotsDashesAndUnderscores) {
  EXPECT_TRUE(Trunk::valid_name("acme"));
  EXPECT_TRUE(Trunk::valid_name("Acme-UK_2.backup"));
  EXPECT_FALSE(Trunk::valid_name(""));
  EXPECT_FALSE(Trunk::valid_name("acme uk"));
  EXPECT_FALSE(Trunk::valid_name("acme/uk"));
  EXPECT_FALSE(Trunk::valid_name(std::string(65, 'a')));
}

TEST(TrunkTest, ARangeIsAnAddressOrACidrBlock) {
  EXPECT_TRUE(Trunk::valid_range("203.0.113.0/24"));
  EXPECT_TRUE(Trunk::valid_range("203.0.113.7"));
  EXPECT_TRUE(Trunk::valid_range("2001:db8::/32"));
  EXPECT_FALSE(Trunk::valid_range("203.0.113.0/33"));
  EXPECT_FALSE(Trunk::valid_range("203.0.113.0/"));
  EXPECT_FALSE(Trunk::valid_range("sip.acme.example"));
}

// A request from inside one of the trunk's ranges is the trunk; from outside, it is not.
TEST(TrunkTest, ARequestFromItsRangesIsTheTrunk) {
  Trunk trunk;
  trunk.inbound_addresses = {"203.0.113.0/24", "198.51.100.9", "2001:db8::/32"};

  EXPECT_TRUE(trunk.admits("203.0.113.200"));
  EXPECT_TRUE(trunk.admits("198.51.100.9"));
  EXPECT_TRUE(trunk.admits("2001:db8::1"));
  EXPECT_FALSE(trunk.admits("203.0.114.1"));
  EXPECT_FALSE(trunk.admits("198.51.100.10"));
  EXPECT_FALSE(trunk.admits("2001:db9::1"));
  EXPECT_FALSE(trunk.admits("not an address"));

  // A dual-stack socket reports an IPv4 peer as an IPv4-mapped IPv6 address.
  EXPECT_TRUE(trunk.admits("::ffff:203.0.113.5"));
}

TEST(TrunkTest, ItsStoredFormRoundTrips) {
  Trunk trunk;
  trunk.name = "acme";
  trunk.uri = "sip:sip.acme.example";
  trunk.password = "s3cret";
  trunk.register_enabled = true;
  trunk.inbound_addresses = {"203.0.113.0/24"};
  trunk.attributes["country"] = "44";

  const auto back = Trunk::from_json(trunk.to_json());
  EXPECT_EQ(back.name, "acme");
  EXPECT_EQ(back.password, "s3cret");
  EXPECT_TRUE(back.register_enabled);
  EXPECT_EQ(back.register_expires, 300u);
  EXPECT_EQ(back.inbound_addresses, trunk.inbound_addresses);
  EXPECT_EQ(back.attributes, trunk.attributes);

  EXPECT_THROW(Trunk::from_json(boost::json::object{{"uri", "sip:x"}}), std::runtime_error);
}
