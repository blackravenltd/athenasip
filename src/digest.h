//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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

// RFC 3261 22.2 and 22.3 from the client's side, for this node answering a server that challenged it (a carrier on
// a trunk): the credentials answering one challenge for a request to `uri`. MD5 and SHA-256, plain or -sess
// (RFC 7616 3.4.2); qop=auth when the challenge offers it, which is required of a server offering qop at all.
// cnonce and nc are the caller's: nc counts the requests sent under one nonce, from 1.
//
// Nothing when the challenge is not Digest, names an algorithm this node does not have, or offers only auth-int.
std::optional<types::Authorization> respond(const types::Authorization& challenge, const std::string& username, const std::string& password,
                                            const std::string& method, const std::string& uri, const std::string& cnonce, std::uint32_t nc);

// RFC 8760 section 2.4: of several challenges for one realm, the one to answer: SHA-256 before MD5.
std::optional<types::Authorization> preferred(const std::vector<types::Authorization>& challenges);

}  // namespace athenasip::digest
