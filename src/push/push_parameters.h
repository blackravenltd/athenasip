//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "../types/sip_uri.h"
#include "push_service.h"

namespace athenasip::push {

// RFC 8599 8.7: the pn-* parameters of a contact, when it has a pn-provider. An empty provider is a query for
// every supported service (4.1.5); an empty prid is a query, not a request for push.
inline std::optional<Notification> notification_of(const types::SIPUri& contact) {
  if (!contact.has_parameter("pn-provider")) return std::nullopt;

  Notification notification;
  notification.provider = contact.parameter("pn-provider");
  notification.prid = contact.parameter("pn-prid");
  notification.param = contact.parameter("pn-param");
  return notification;
}

// RFC 8599 5.3: two contacts belong to the same push registration when pn-provider, pn-prid and pn-param all
// match; one present in one URI and absent in the other is no match.
inline bool same_push_parameters(const types::SIPUri& a, const types::SIPUri& b) {
  for (const std::string_view name : {"pn-provider", "pn-prid", "pn-param"}) {
    if (a.has_parameter(name) != b.has_parameter(name)) return false;
    if (a.parameter(name) != b.parameter(name)) return false;
  }
  return a.has_parameter("pn-provider");
}

// RFC 8599 section 13: a contact as anyone other than its own client may see it, without the pn-* parameters.
inline types::SIPUri without_push_parameters(types::SIPUri contact) {
  for (const std::string_view name : {"pn-provider", "pn-prid", "pn-param", "pn-purr"}) contact.remove_parameter(name);
  return contact;
}

}  // namespace athenasip::push
