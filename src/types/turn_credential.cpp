//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "types/turn_credential.h"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cstdint>
#include <vector>

namespace athenasip::types {

namespace {

const char* kBase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Standard base64 with padding, as the scheme specifies; not the URL-safe alphabet.
std::string base64_encode(const std::vector<unsigned char>& input) {
  std::string out;
  out.reserve(((input.size() + 2) / 3) * 4);

  for (std::size_t i = 0; i < input.size(); i += 3) {
    const std::uint32_t a = input[i];
    const std::uint32_t b = i + 1 < input.size() ? input[i + 1] : 0;
    const std::uint32_t c = i + 2 < input.size() ? input[i + 2] : 0;
    const std::uint32_t triple = (a << 16) | (b << 8) | c;

    out += kBase64[(triple >> 18) & 0x3F];
    out += kBase64[(triple >> 12) & 0x3F];
    out += i + 1 < input.size() ? kBase64[(triple >> 6) & 0x3F] : '=';
    out += i + 2 < input.size() ? kBase64[triple & 0x3F] : '=';
  }

  return out;
}

// A name that is safe in a TURN username. coturn parses the username as <expiry>[:<name>] and refuses the whole
// credential if the name has, say, a space in it, with an error that looks like a bad secret. The name is only for
// tying a relay session to its requester in a log, so unsafe characters are dropped.
std::string safe_name(const std::string& name) {
  std::string out;

  for (const char c : name) {
    if (out.size() >= 32) break;

    const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    if (safe) out.push_back(c);
  }

  return out;
}

}  // namespace

TurnCredential TurnCredential::issue(const std::string& secret, const std::string& name, std::time_t now, std::uint32_t ttl) {
  TurnCredential credential;

  // No secret means TURN is not configured: an empty credential, not an error.
  if (secret.empty() || ttl == 0) return credential;

  credential.expires_at = now + static_cast<std::time_t>(ttl);
  credential.username = std::to_string(credential.expires_at);

  // Nothing left after filtering means no name, which is still a valid credential.
  const auto suffix = safe_name(name);
  if (!suffix.empty()) credential.username += ":" + suffix;

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;

  if (HMAC(EVP_sha1(), secret.data(), static_cast<int>(secret.size()), reinterpret_cast<const unsigned char*>(credential.username.data()),
           credential.username.size(), digest, &length) == nullptr) {
    return TurnCredential{};
  }

  credential.password = base64_encode(std::vector<unsigned char>(digest, digest + length));

  return credential;
}

}  // namespace athenasip::types
