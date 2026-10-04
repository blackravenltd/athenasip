//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "jwt.h"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/pem.h>

#include <array>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace athenasip::push::jwt {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int decoded_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

// Never prompts: a key with a passphrase is a configuration mistake here, not a question
// for whoever is at the terminal.
int no_passphrase(char*, int, int, void*) { return 0; }

bool is_p256(EVP_PKEY* key) {
  char group[64] = {};
  std::size_t length = 0;
  if (EVP_PKEY_get_group_name(key, group, sizeof(group), &length) != 1) return false;
  return std::strcmp(group, "prime256v1") == 0 || std::strcmp(group, "P-256") == 0;
}

std::optional<std::string> digest_sign(EVP_PKEY* key, std::string_view input) {
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, key) != 1) return std::nullopt;

  const auto* data = reinterpret_cast<const unsigned char*>(input.data());
  std::size_t length = 0;
  if (EVP_DigestSign(context.get(), nullptr, &length, data, input.size()) != 1) return std::nullopt;

  std::string signature(length, '\0');
  if (EVP_DigestSign(context.get(), reinterpret_cast<unsigned char*>(signature.data()), &length, data, input.size()) != 1) return std::nullopt;
  signature.resize(length);
  return signature;
}

// RFC 7518 3.4: JWS carries ECDSA as R || S, each left-padded to the curve's 32 bytes,
// where OpenSSL produces the DER SEQUENCE of X9.62.
std::optional<std::string> der_to_raw(const std::string& der) {
  const auto* cursor = reinterpret_cast<const unsigned char*>(der.data());
  std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)> parsed(d2i_ECDSA_SIG(nullptr, &cursor, static_cast<long>(der.size())), ECDSA_SIG_free);
  if (!parsed) return std::nullopt;

  const BIGNUM* r = nullptr;
  const BIGNUM* s = nullptr;
  ECDSA_SIG_get0(parsed.get(), &r, &s);

  std::array<unsigned char, 64> raw{};
  if (BN_bn2binpad(r, raw.data(), 32) != 32 || BN_bn2binpad(s, raw.data() + 32, 32) != 32) return std::nullopt;
  return std::string(reinterpret_cast<const char*>(raw.data()), raw.size());
}

}  // namespace

std::string base64url_encode(std::string_view bytes) {
  std::string out;
  out.reserve((bytes.size() * 4 + 2) / 3);

  std::size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const auto n = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8) | static_cast<unsigned char>(bytes[i + 2]);
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
    out += kAlphabet[n & 63];
  }

  if (bytes.size() - i == 1) {
    const auto n = static_cast<unsigned char>(bytes[i]) << 16;
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
  } else if (bytes.size() - i == 2) {
    const auto n = (static_cast<unsigned char>(bytes[i]) << 16) | (static_cast<unsigned char>(bytes[i + 1]) << 8);
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
  }

  return out;
}

std::optional<std::string> base64url_decode(std::string_view text) {
  // A single character left over carries six bits, which is not a byte.
  if (text.size() % 4 == 1) return std::nullopt;

  std::string out;
  out.reserve(text.size() * 3 / 4);

  unsigned int buffer = 0;
  int bits = 0;
  for (const char c : text) {
    const int value = decoded_value(c);
    if (value < 0) return std::nullopt;

    buffer = (buffer << 6) | static_cast<unsigned int>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((buffer >> bits) & 0xff);
    }
  }

  return out;
}

Key read_private_key(const std::string& pem) {
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
  if (!bio) return nullptr;

  EVP_PKEY* key = PEM_read_bio_PrivateKey(bio.get(), nullptr, no_passphrase, nullptr);
  return key ? Key(key, EVP_PKEY_free) : nullptr;
}

Key read_private_key_file(const std::string& path, std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "cannot read " + path;
    return nullptr;
  }

  std::stringstream contents;
  contents << file.rdbuf();

  auto key = read_private_key(contents.str());
  if (!key) error = path + " holds no private key in PEM (or one with a passphrase)";
  return key;
}

std::string sign(const Key& key, std::string_view header_json, std::string_view claims_json) {
  if (!key) return {};

  const std::string input = base64url_encode(header_json) + "." + base64url_encode(claims_json);

  std::optional<std::string> signature;
  switch (EVP_PKEY_get_base_id(key.get())) {
    case EVP_PKEY_EC:
      if (!is_p256(key.get())) return {};
      if (auto der = digest_sign(key.get(), input)) signature = der_to_raw(*der);
      break;
    case EVP_PKEY_RSA:
      signature = digest_sign(key.get(), input);
      break;
    default:
      return {};
  }

  if (!signature) return {};
  return input + "." + base64url_encode(*signature);
}

}  // namespace athenasip::push::jwt
