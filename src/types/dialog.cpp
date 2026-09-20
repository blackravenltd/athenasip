//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "dialog.h"

namespace athenasip::types {

std::string Dialog::id() const {
  if (call_id.empty() || caller_tag.empty() || callee_tag.empty()) return "";

  // The separators matter for the same reason they do in a transaction key: without
  // them two different pairs of tags can spell the same string.
  return call_id + "|" + caller_tag + "|" + callee_tag;
}

bool Dialog::matches(const std::string& message_call_id, const std::string& from_tag, const std::string& to_tag) const {
  if (message_call_id != call_id) return false;

  // From the caller: its own tag in From, the callee's in To. From the callee: the
  // reverse. RFC 3261 12.2.2 states this from one UA's side, where the two are local
  // and remote; a proxy sees both and has to accept either order.
  if (from_tag == caller_tag && to_tag == callee_tag) return true;
  if (from_tag == callee_tag && to_tag == caller_tag) return true;

  return false;
}

bool Dialog::is_from_caller(const std::string& from_tag) const { return !caller_tag.empty() && from_tag == caller_tag; }

}  // namespace athenasip::types
