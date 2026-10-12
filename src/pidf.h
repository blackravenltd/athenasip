//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <string>
#include <vector>

namespace athenasip::pidf {

// RFC 3863 presence documents, as the notifier composes them (docs/design/presence.md). Nothing here interprets
// presence: a publication's elements are carried as they were published.

// One document for the presentity from the PIDF documents it published, oldest first: every tuple, note and
// extension element of each, with the namespaces they were published under. An element with an `id` that a later
// publication also has is taken from the later. A document that is not PIDF contributes nothing.
std::string compose(const std::string& entity, const std::vector<std::string>& documents);

// What a presentity that has published nothing is: one tuple, open while it has a binding and closed when it has
// none, with no contact.
std::string registration(const std::string& entity, bool registered);

// "open" when any tuple's basic status is (RFC 3863 4.1.4), else "closed": the one word a console shows.
std::string basic(const std::string& document);

}  // namespace athenasip::pidf
