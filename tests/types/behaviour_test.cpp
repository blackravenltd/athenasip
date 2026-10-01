//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include "types/realm.h"

using athenasip::types::Behaviour;
using athenasip::types::MediaPolicy;

// The 2026-10-01 decision: the shipped default is what the standards say, apart from one
// deliberate and recorded deviation - anchoring media, as most servers fronting rtpengine
// do. Within that, a leg's description passes through as the leg wrote it: no profile is
// imposed on a callee that has not asked for one.
TEST(BehaviourTest, TheShippedDefaultAnchorsAndPassesEachLegsProfileThrough) {
  const MediaPolicy shipped;

  EXPECT_TRUE(shipped.anchor);
  EXPECT_EQ(shipped.profiles, MediaPolicy::Profiles::Mirror);
}

// A realm that says nothing takes the server's default for everything, so changing the
// server's default changes every realm that has not chosen otherwise.
TEST(BehaviourTest, ARealmThatSaysNothingIsTheServerDefault) {
  MediaPolicy server;
  server.anchor = false;
  server.profiles = MediaPolicy::Profiles::FromTransport;

  const Behaviour silent;
  const auto effective = silent.over(server);

  EXPECT_FALSE(effective.anchor);
  EXPECT_EQ(effective.profiles, MediaPolicy::Profiles::FromTransport);
}

// And a realm that sets one setting overrides that one and inherits the rest.
TEST(BehaviourTest, ARealmOverridesOnlyWhatItSets) {
  MediaPolicy server;
  server.anchor = true;
  server.profiles = MediaPolicy::Profiles::Mirror;

  Behaviour realm;
  realm.media_profile = MediaPolicy::Profiles::WebRtc;

  const auto effective = realm.over(server);

  EXPECT_TRUE(effective.anchor) << "inherited";
  EXPECT_EQ(effective.profiles, MediaPolicy::Profiles::WebRtc) << "overridden";
}

// The probe interval is decided at registration, where the realm is in hand, so it is
// resolved on its own rather than kept on a call.
TEST(BehaviourTest, ARealmsQualifyIntervalOverridesTheServers) {
  Behaviour silent;
  EXPECT_EQ(silent.qualify_over(60), 60u);

  Behaviour off;
  off.qualify_interval = 0;
  EXPECT_EQ(off.qualify_over(60), 0u);

  Behaviour often;
  often.qualify_interval = 25;
  EXPECT_EQ(often.qualify_over(0), 25u);

  EXPECT_TRUE(Behaviour::valid_qualify_interval(0));
  EXPECT_TRUE(Behaviour::valid_qualify_interval(5));
  EXPECT_TRUE(Behaviour::valid_qualify_interval(86400));
  EXPECT_FALSE(Behaviour::valid_qualify_interval(4));
  EXPECT_FALSE(Behaviour::valid_qualify_interval(86401));
}

// A name in a config file or an API body is either one of these or a mistake, and a
// mistake is reported rather than read as something else.
TEST(BehaviourTest, AProfileNameIsReadStrictly) {
  EXPECT_EQ(MediaPolicy::parse_profiles("mirror"), MediaPolicy::Profiles::Mirror);
  EXPECT_EQ(MediaPolicy::parse_profiles("transport"), MediaPolicy::Profiles::FromTransport);
  EXPECT_EQ(MediaPolicy::parse_profiles(" WebRTC "), MediaPolicy::Profiles::WebRtc);
  EXPECT_EQ(MediaPolicy::parse_profiles("rtp"), MediaPolicy::Profiles::PlainRtp);
  EXPECT_EQ(MediaPolicy::parse_profiles("srtp"), MediaPolicy::Profiles::SrtpSdes);

  EXPECT_FALSE(MediaPolicy::parse_profiles("web-rtc").has_value());
  EXPECT_FALSE(MediaPolicy::parse_profiles("").has_value());
}
