//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "types/password.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstring>
#include <sstream>
#include <vector>

namespace athenasip::types {

namespace {

constexpr const char* kAlgorithm = "pbkdf2-sha256";
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHashBytes = 32;

const char* kBase64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

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

bool base64_decode(const std::string& input, std::vector<unsigned char>& out) {
  if (input.size() % 4 != 0 || input.empty()) return false;

  std::array<int, 256> value{};
  value.fill(-1);
  for (int i = 0; i < 64; ++i) value[static_cast<unsigned char>(kBase64[i])] = i;

  out.clear();
  out.reserve((input.size() / 4) * 3);

  for (std::size_t i = 0; i < input.size(); i += 4) {
    std::array<int, 4> quad{};

    for (std::size_t j = 0; j < 4; ++j) {
      const auto character = static_cast<unsigned char>(input[i + j]);

      if (character == '=') {
        // Padding, and only ever at the end.
        if (i + 4 < input.size() || j < 2) return false;
        quad[j] = 0;
        continue;
      }

      quad[j] = value[character];
      if (quad[j] < 0) return false;
    }

    const std::uint32_t triple = (static_cast<std::uint32_t>(quad[0]) << 18) | (static_cast<std::uint32_t>(quad[1]) << 12) |
                                 (static_cast<std::uint32_t>(quad[2]) << 6) | static_cast<std::uint32_t>(quad[3]);

    out.push_back(static_cast<unsigned char>((triple >> 16) & 0xFF));
    if (input[i + 2] != '=') out.push_back(static_cast<unsigned char>((triple >> 8) & 0xFF));
    if (input[i + 3] != '=') out.push_back(static_cast<unsigned char>(triple & 0xFF));
  }

  return true;
}

// The derivation itself, shared by hashing and verifying so the two can never disagree
// about what they compute.
bool derive(const std::string& password, const std::vector<unsigned char>& salt, std::uint32_t iterations, std::size_t length,
            std::vector<unsigned char>& out) {
  out.assign(length, 0);

  return PKCS5_PBKDF2_HMAC(password.data(), static_cast<int>(password.size()), salt.data(), static_cast<int>(salt.size()), static_cast<int>(iterations),
                           EVP_sha256(), static_cast<int>(length), out.data()) == 1;
}

std::vector<std::string> split(const std::string& value, char separator) {
  std::vector<std::string> parts;
  std::istringstream stream(value);
  std::string part;

  while (std::getline(stream, part, separator)) parts.push_back(part);

  return parts;
}

}  // namespace

std::string Password::hash(const std::string& password, std::uint32_t iterations) {
  if (password.empty() || iterations == 0) return {};

  std::vector<unsigned char> salt(kSaltBytes);
  if (RAND_bytes(salt.data(), static_cast<int>(salt.size())) != 1) return {};

  std::vector<unsigned char> derived;
  if (!derive(password, salt, iterations, kHashBytes, derived)) return {};

  return std::string(kAlgorithm) + "$" + std::to_string(iterations) + "$" + base64_encode(salt) + "$" + base64_encode(derived);
}

bool Password::verify(const std::string& password, const std::string& stored) {
  if (password.empty() || stored.empty()) return false;

  const auto parts = split(stored, '$');
  if (parts.size() != 4) return false;
  if (parts[0] != kAlgorithm) return false;

  std::uint32_t iterations = 0;
  try {
    // Refuses anything that is not entirely digits: stoul would accept "600000x" and
    // a leading sign, and a cost read from a corrupted row should not be guessed at.
    if (parts[1].empty() || parts[1].find_first_not_of("0123456789") != std::string::npos) return false;
    iterations = static_cast<std::uint32_t>(std::stoul(parts[1]));
  } catch (const std::exception&) {
    return false;
  }

  if (iterations == 0) return false;

  std::vector<unsigned char> salt;
  std::vector<unsigned char> expected;
  if (!base64_decode(parts[2], salt) || !base64_decode(parts[3], expected)) return false;
  if (salt.empty()) return false;

  // Exactly the length this code writes, and derive to that length rather than to the
  // stored one. PBKDF2 to a shorter length is a *prefix* of the output for a longer one -
  // that is the construction, not a quirk of OpenSSL - so deriving to the stored hash's own
  // length compares only as many bytes as the record happens to carry. A hash truncated to
  // one byte was then matched by about one password in 256.
  //
  // A row gets truncated by a partial write, a corrupted value, a migration, or by somebody
  // with write access to the datastore who cannot read the hash but can shorten it. The
  // last is the one that matters: it turns write access into a login, quietly, leaving a
  // record that still parses.
  if (expected.size() != kHashBytes) return false;

  std::vector<unsigned char> derived;
  if (!derive(password, salt, iterations, kHashBytes, derived)) return false;

  // Constant time, so the comparison says nothing about how much of the hash matched.
  return CRYPTO_memcmp(derived.data(), expected.data(), kHashBytes) == 0;
}

}  // namespace athenasip::types
