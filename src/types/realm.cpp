//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "realm.h"

#include <regex>
#include <sstream>

#include "../util.h"

namespace athenasip::types {

Realm::Realm() {}

Realm::Realm(const std::string& _name) { name = _name; }

std::string Realm::to_string() const { return name; }

}  // namespace athenasip::types
