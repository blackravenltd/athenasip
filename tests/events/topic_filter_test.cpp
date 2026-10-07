//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include "events/event_system.h"

using athenasip::events::TopicFilter;

// is_filter

TEST(TopicFilterTest, IsFilterDetectsWildcards) {
  EXPECT_TRUE(TopicFilter::is_filter("nodes/+/status"));
  EXPECT_TRUE(TopicFilter::is_filter("nodes/#"));
  EXPECT_FALSE(TopicFilter::is_filter("nodes/sip-0001/status"));
  EXPECT_FALSE(TopicFilter::is_filter(""));
}

// is_valid_topic

TEST(TopicFilterTest, ValidTopicsCarryNoWildcards) {
  EXPECT_TRUE(TopicFilter::is_valid_topic("nodes/sip-0001/status"));
  EXPECT_TRUE(TopicFilter::is_valid_topic("a"));
  EXPECT_FALSE(TopicFilter::is_valid_topic("nodes/+/status"));
  EXPECT_FALSE(TopicFilter::is_valid_topic("nodes/#"));
  EXPECT_FALSE(TopicFilter::is_valid_topic(""));
}

// is_valid_filter

TEST(TopicFilterTest, ValidFilterAcceptsWellFormedWildcards) {
  EXPECT_TRUE(TopicFilter::is_valid_filter("nodes/sip-0001/status"));
  EXPECT_TRUE(TopicFilter::is_valid_filter("nodes/+/status"));
  EXPECT_TRUE(TopicFilter::is_valid_filter("nodes/#"));
  EXPECT_TRUE(TopicFilter::is_valid_filter("#"));
  EXPECT_TRUE(TopicFilter::is_valid_filter("+"));
  EXPECT_TRUE(TopicFilter::is_valid_filter("+/+/+"));
}

TEST(TopicFilterTest, MultiLevelWildcardMustBeLastAndAlone) {
  EXPECT_FALSE(TopicFilter::is_valid_filter("nodes/#/status"));
  EXPECT_FALSE(TopicFilter::is_valid_filter("nodes/a#"));
  EXPECT_FALSE(TopicFilter::is_valid_filter("#/nodes"));
}

TEST(TopicFilterTest, SingleLevelWildcardMustOccupyWholeLevel) {
  EXPECT_FALSE(TopicFilter::is_valid_filter("nodes/a+/status"));
  EXPECT_FALSE(TopicFilter::is_valid_filter("nodes/+a/status"));
}

TEST(TopicFilterTest, EmptyFilterIsInvalid) { EXPECT_FALSE(TopicFilter::is_valid_filter("")); }

// matches

TEST(TopicFilterTest, ExactMatch) {
  EXPECT_TRUE(TopicFilter::matches("nodes/sip-0001/status", "nodes/sip-0001/status"));
  EXPECT_FALSE(TopicFilter::matches("nodes/sip-0001/status", "nodes/sip-0002/status"));
}

TEST(TopicFilterTest, SingleLevelWildcardMatchesExactlyOneLevel) {
  EXPECT_TRUE(TopicFilter::matches("nodes/+/status", "nodes/sip-0001/status"));
  EXPECT_FALSE(TopicFilter::matches("nodes/+/status", "nodes/sip-0001/extra/status"));
  EXPECT_FALSE(TopicFilter::matches("nodes/+/status", "nodes/status"));
}

TEST(TopicFilterTest, MultiLevelWildcardMatchesRemainder) {
  EXPECT_TRUE(TopicFilter::matches("nodes/#", "nodes/sip-0001"));
  EXPECT_TRUE(TopicFilter::matches("nodes/#", "nodes/sip-0001/channels/udp"));
  EXPECT_TRUE(TopicFilter::matches("#", "anything/at/all"));
}

TEST(TopicFilterTest, LevelCountMustMatchWithoutMultiLevelWildcard) {
  EXPECT_FALSE(TopicFilter::matches("nodes/sip-0001", "nodes/sip-0001/status"));
  EXPECT_FALSE(TopicFilter::matches("nodes/sip-0001/status", "nodes/sip-0001"));
}

// A trailing '#' under the subscriber prefix matches every sibling topic.
TEST(TopicFilterTest, SubscriberHashFilterMatchesStatusAndInvite) {
  const std::string hash = "subscribers/sip:alice@example.com/#";
  EXPECT_TRUE(TopicFilter::matches(hash, "subscribers/sip:alice@example.com/status"));
  EXPECT_TRUE(TopicFilter::matches(hash, "subscribers/sip:alice@example.com/invite"));

  // A narrow filter takes the invite topic only.
  const std::string invite = "subscribers/sip:alice@example.com/invite";
  EXPECT_TRUE(TopicFilter::matches(invite, "subscribers/sip:alice@example.com/invite"));
  EXPECT_FALSE(TopicFilter::matches(invite, "subscribers/sip:alice@example.com/status"));
}

TEST(TopicFilterTest, InvalidArgumentsNeverMatch) {
  EXPECT_FALSE(TopicFilter::matches("nodes/#/status", "nodes/sip-0001/status"));
  EXPECT_FALSE(TopicFilter::matches("nodes/+/status", "nodes/+/status"));
  EXPECT_FALSE(TopicFilter::matches("", "nodes/sip-0001"));
  EXPECT_FALSE(TopicFilter::matches("nodes/#", ""));
}

TEST(TopicFilterTest, DollarTopicsAreNotMatchedByLeadingWildcard) {
  EXPECT_FALSE(TopicFilter::matches("#", "$SYS/broker/uptime"));
  EXPECT_FALSE(TopicFilter::matches("+/broker/uptime", "$SYS/broker/uptime"));
  EXPECT_TRUE(TopicFilter::matches("$SYS/#", "$SYS/broker/uptime"));
}
