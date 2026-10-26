//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// The dialog as the call record keeps it (all state lives in the datastore, 2026-10-11).
#include "types/dialog.h"

#include <gtest/gtest.h>

#include <memory>

#include "types/sip_identity.h"
#include "types/sip_uri.h"

using namespace athenasip;
using athenasip::types::Dialog;

// Everything a node needs to carry a dialog on survives the record: the identity, both ends, their targets, the
// route set, the sequence numbers, the session timer on the wall clock, and the trunk's CSeq offset.
TEST(DialogTest, ADialogRoundTripsThroughItsRecord) {
  Dialog dialog;
  dialog.call_id = "call-1";
  dialog.caller_tag = "alice";
  dialog.callee_tag = "bob";
  dialog.from_node = "node-a";
  dialog.caller = std::make_shared<types::SIPIdentity>("sip:alice@example.com");
  dialog.callee = std::make_shared<types::SIPIdentity>("sip:bob@example.com");
  dialog.caller_target = std::make_shared<types::SIPUri>("sip:alice@192.0.2.10:5060");
  dialog.callee_target = std::make_shared<types::SIPUri>("sip:bob@192.0.2.20:5060;transport=tcp");
  dialog.route_set = {std::make_shared<types::SIPUri>("sip:abc@192.0.2.1:5060;lr"), std::make_shared<types::SIPUri>("sip:def@192.0.2.1:5060;lr")};
  dialog.caller_cseq = 7;
  dialog.callee_cseq = 3;
  dialog.secure = true;
  dialog.state = Dialog::State::Confirmed;
  dialog.created_at = 1000;
  dialog.confirmed_at = 1005;
  dialog.session_interval = 1800;
  dialog.refresher = "uac";
  dialog.session_expires_at = 2805;
  dialog.trunk = "acme";
  dialog.cseq_offset = 2;
  dialog.revision = 9;

  const auto back = Dialog::from_json(dialog.to_json());
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->id(), "call-1|alice|bob");
  EXPECT_EQ(back->from_node, "node-a");
  ASSERT_NE(back->caller, nullptr);
  EXPECT_EQ(back->caller->uri->to_string(), "sip:alice@example.com");
  ASSERT_NE(back->callee_target, nullptr);
  EXPECT_EQ(back->callee_target->to_string(), "sip:bob@192.0.2.20:5060;transport=tcp");
  ASSERT_EQ(back->route_set.size(), 2u);
  EXPECT_EQ(back->route_set[1]->to_string(), "sip:def@192.0.2.1:5060;lr");
  EXPECT_EQ(back->caller_cseq, 7u);
  EXPECT_EQ(back->callee_cseq, 3u);
  EXPECT_TRUE(back->secure);
  EXPECT_EQ(back->state, Dialog::State::Confirmed);
  EXPECT_EQ(back->created_at, 1000);
  EXPECT_EQ(back->confirmed_at, 1005);
  EXPECT_EQ(back->session_interval, 1800u);
  EXPECT_EQ(back->refresher, "uac");
  EXPECT_EQ(back->session_expires_at, 2805);
  EXPECT_EQ(back->trunk, "acme");
  EXPECT_EQ(back->cseq_offset, 2u);
  EXPECT_EQ(back->revision, 9u);
  EXPECT_TRUE(back->matches("call-1", "bob", "alice"));
}

// A record from before the dialog was kept whole, or one with nothing set, reads as an early dialog with no ends.
TEST(DialogTest, AnEmptyRecordIsAnEarlyDialogWithNothingSet) {
  const auto back = Dialog::from_json(boost::json::object{});
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(back->state, Dialog::State::Early);
  EXPECT_EQ(back->caller, nullptr);
  EXPECT_TRUE(back->route_set.empty());
  EXPECT_EQ(back->session_interval, 0u);
}
