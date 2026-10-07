//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <openssl/evp.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace athenasip::push::jwt {

using Key = std::shared_ptr<EVP_PKEY>;

// RFC 7515 2 and appendix C: base64 with the URL-safe alphabet and no padding.
std::string base64url_encode(std::string_view bytes);
std::optional<std::string> base64url_decode(std::string_view text);

// A private key from PEM text (PKCS#8 or the traditional forms); null when there is none.
Key read_private_key(const std::string& pem);
Key read_private_key_file(const std::string& path, std::string& error);

// A compact JWS (RFC 7515 7.1) over the two JSON texts, as given. The algorithm follows
// the key: ES256 for P-256 (RFC 7518 3.4, the signature as R || S, 64 bytes), RS256 for
// RSA (3.3). Empty when the key is neither or signing fails.
std::string sign(const Key& key, std::string_view header_json, std::string_view claims_json);

}  // namespace athenasip::push::jwt
