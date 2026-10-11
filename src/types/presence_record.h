//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <ctime>
#include <string>

namespace athenasip::types {

// What the notifier keeps about a presentity (docs/design/presence.md): a subscription to it (RFC 6665), or a
// publication of its state (RFC 3903). The store holds it, indexes it and expires it; it reads none of the body.
struct PresenceRecord {
  enum class Kind {
    Subscription,
    Publication,
  };

  Kind kind = Kind::Subscription;

  // The subscription's ID, or the publication's entity tag.
  std::string id;

  // The presentity's address of record, as the notifier writes it. The store compares it, never interprets it.
  std::string aor;

  // A subscription's holder: the node with the watcher's flow, which alone sends its NOTIFYs. Empty for a
  // publication, which any node may refresh.
  std::string node_id;

  std::string content_type;
  std::string body;

  // Filled by the store from the lifetime it was given.
  std::time_t expires_at = 0;

  static const char* to_string(Kind kind) { return kind == Kind::Subscription ? "subscription" : "publication"; }
};

}  // namespace athenasip::types
