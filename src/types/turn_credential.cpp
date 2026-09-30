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

// Standard base64 with padding, which is what the scheme specifies and what every TURN
// server implementing it expects. Not the URL-safe alphabet: the value travels in a JSON
// body and in a WebRTC configuration object, never in a path.
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

// A name that is safe in a TURN username. coturn parses the username as
// <expiry>[:<name>] and refuses the whole credential when the name is not one it accepts:
// a space in it is answered 401 "wrong username" and then 400, which looks from the client
// side exactly like a bad secret and is not.
//
// So the name is filtered here rather than trusted, because the alternative is a caller
// passing something human-readable - which is what happened - and a relay that fails with
// a misleading error. Anything left out is only decoration: coturn does not look at the
// name, and it is here so a relay session can be tied back to whoever asked in a log.
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

  // No secret is not an error here. A deployment with no TURN server has nothing to
  // authenticate to, and the endpoint above says so by handing back no credentials rather
  // than by failing.
  if (secret.empty() || ttl == 0) return credential;

  credential.expires_at = now + static_cast<std::time_t>(ttl);
  credential.username = std::to_string(credential.expires_at);

  // Filtered rather than trusted - see safe_name. Nothing left after filtering means no
  // name at all, which is a valid credential, rather than a colon with nothing after it.
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
