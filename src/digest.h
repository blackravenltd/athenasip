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
#include "types/authorization.h"
#include "types/subscriber.h"

namespace athenasip::digest {

// RFC 3261 22 Digest authentication, shared by the registrar (401, WWW-Authenticate) and
// the proxy (407, Proxy-Authenticate).

// Whether the credentials carry everything verify needs. If not, challenge again rather
// than refuse.
bool is_complete(const std::shared_ptr<types::Authorization>& credentials);

// Returns empty when the credentials match the subscriber's stored HA1 for the method,
// otherwise the reason. The caller checks the nonce first.
std::string verify(const types::Subscriber& subscriber, types::Authorization& credentials, const std::string& method);

// RFC 8760 section 2.1: one challenge per algorithm, most preferred first, into `field`.
void add_challenges(SIPHeader& header, const std::string& field, const std::string& realm, const std::string& nonce);

}  // namespace athenasip::digest
