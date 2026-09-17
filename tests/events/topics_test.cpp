//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include <gtest/gtest.h>

#include <vector>

#include "events/event_system.h"
#include "events/topics.h"

using athenasip::events::TopicFilter;
namespace topics = athenasip::events::topics;

namespace {

std::vector<std::string> every_topic() {
  return {
      topics::node_status("sip-0001"),
      topics::node_channel("sip-0001", "tcp", "192.0.2.10:5060"),
      topics::node_transaction("sip-0001", "z9hG4bK-abc|host|INVITE"),
      topics::subscriber_status("sip:alice@example.com"),
      topics::subscriber_invite("sip:alice@example.com"),
      topics::call_register("call-1234"),
      topics::call_unregister("call-1234"),
  };
}

}  // namespace

// A leading '/' is an empty first level in MQTT, so "/nodes/x" is a different topic
// from "nodes/x" and a "nodes/#" filter does not match it.
TEST(TopicsTest, NoTopicBeginsWithASlash) {
  for (const auto& topic : every_topic()) EXPECT_NE(topic.front(), '/') << topic;
}

TEST(TopicsTest, EveryTopicIsAValidPublishTopic) {
  for (const auto& topic : every_topic()) EXPECT_TRUE(TopicFilter::is_valid_topic(topic)) << topic;
}

TEST(TopicsTest, NodeTopicsSitUnderTheNodeFilter) {
  EXPECT_TRUE(TopicFilter::matches("nodes/#", topics::node_status("sip-0001")));
  EXPECT_TRUE(TopicFilter::matches("nodes/#", topics::node_channel("sip-0001", "tcp", "192.0.2.10:5060")));
  EXPECT_TRUE(TopicFilter::matches("nodes/#", topics::node_transaction("sip-0001", "t-1")));
  EXPECT_TRUE(TopicFilter::matches("nodes/+/status", topics::node_status("sip-0001")));
}

TEST(TopicsTest, CallTopicsSitUnderTheCallFilter) {
  EXPECT_TRUE(TopicFilter::matches("calls/#", topics::call_register("call-1234")));
  EXPECT_TRUE(TopicFilter::matches("calls/#", topics::call_unregister("call-1234")));
  EXPECT_TRUE(TopicFilter::matches("calls/+/unregister", topics::call_unregister("call-1234")));
}

// The invite subscription must not receive the node's own status publish for the same
// subscriber, which is what a trailing '#' filter used to do.
TEST(TopicsTest, SubscriberInviteAndStatusAreDistinct) {
  const auto uri = std::string("sip:alice@example.com");

  EXPECT_NE(topics::subscriber_invite(uri), topics::subscriber_status(uri));
  EXPECT_FALSE(TopicFilter::matches(topics::subscriber_invite(uri), topics::subscriber_status(uri)));
}

TEST(TopicsTest, TopicsAreHierarchicalNotDotSeparated) {
  EXPECT_EQ(topics::call_unregister("call-1234"), "calls/call-1234/unregister");
  EXPECT_EQ(topics::node_transaction("sip-0001", "t-1"), "nodes/sip-0001/transactions/t-1");
  EXPECT_EQ(topics::node_channel("sip-0001", "tcp", "192.0.2.10:5060"), "nodes/sip-0001/channels/tcp/192.0.2.10:5060");
}
