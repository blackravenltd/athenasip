//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <string>

#include "sip_header.h"
#include "types/account.h"
#include "types/authorization.h"

namespace athenasip::digest {

// RFC 3261 22: the registrar asks with 401 and WWW-Authenticate, a proxy with 407 and
// Proxy-Authenticate, and the arithmetic underneath is the same. Both use these.

// Whether the credentials carry everything a check needs. Anything less is answered with a
// fresh challenge rather than a refusal: the client may simply not have been asked yet.
bool is_complete(const std::shared_ptr<types::Authorization>& credentials);

// Empty when the credentials verify against the account's stored HA1 for the request
// method, or why they did not. The nonce is the caller's to have checked first.
std::string verify(const types::Account& account, types::Authorization& credentials, const std::string& method);

// RFC 8760 section 2.1: one challenge per algorithm, most preferred first, into `field`.
void add_challenges(SIPHeader& header, const std::string& field, const std::string& realm, const std::string& nonce);

}  // namespace athenasip::digest
