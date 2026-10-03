//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "flow_tokens.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <memory>
#include <stdexcept>
#include <vector>

namespace athenasip {

namespace {

// AES-256-GCM: one primitive for both the sealing and the authentication, and already in
// the OpenSSL this node links. The nonce is random per token, which at 96 bits is safe for
// far more tokens than one process will ever write.
constexpr std::size_t kNonce = 12;
constexpr std::size_t kTag = 16;

using Context = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

Context make_context() {
  Context context(EVP_CIPHER_CTX_new(), &EVP_CIPHER_CTX_free);
  if (!context) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
  return context;
}

// Hex rather than base64: lowercase hex is safe unescaped in a URI user part (RFC 3261
// 25.1) and can never spell an address or a transport name by accident.
std::string to_hex(const std::vector<unsigned char>& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";

  std::string out;
  out.reserve(bytes.size() * 2);

  for (const auto byte : bytes) {
    out += kDigits[byte >> 4];
    out += kDigits[byte & 0x0f];
  }

  return out;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool from_hex(const std::string& text, std::vector<unsigned char>& out) {
  if (text.size() % 2 != 0) return false;

  out.resize(text.size() / 2);

  for (std::size_t i = 0; i < out.size(); ++i) {
    const auto high = hex_value(text[2 * i]);
    const auto low = hex_value(text[2 * i + 1]);
    if (high < 0 || low < 0) return false;

    out[i] = static_cast<unsigned char>((high << 4) | low);
  }

  return true;
}

}  // namespace

FlowTokens::FlowTokens() {
  if (RAND_bytes(_key.data(), static_cast<int>(_key.size())) != 1) throw std::runtime_error("RAND_bytes failed making the flow token key");
}

std::string FlowTokens::seal(const std::string& flow_id) const {
  std::vector<unsigned char> token(kNonce + flow_id.size() + kTag);

  unsigned char* nonce = token.data();
  unsigned char* sealed = nonce + kNonce;
  unsigned char* tag = sealed + flow_id.size();

  if (RAND_bytes(nonce, static_cast<int>(kNonce)) != 1) throw std::runtime_error("RAND_bytes failed sealing a flow token");

  auto context = make_context();
  int length = 0;

  if (EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, _key.data(), nonce) != 1 ||
      EVP_EncryptUpdate(context.get(), sealed, &length, reinterpret_cast<const unsigned char*>(flow_id.data()), static_cast<int>(flow_id.size())) != 1 ||
      EVP_EncryptFinal_ex(context.get(), sealed + length, &length) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTag), tag) != 1) {
    throw std::runtime_error("sealing a flow token failed");
  }

  return to_hex(token);
}

std::string FlowTokens::open(const std::string& token) const {
  std::vector<unsigned char> bytes;
  if (!from_hex(token, bytes) || bytes.size() <= kNonce + kTag) return "";

  const unsigned char* nonce = bytes.data();
  const unsigned char* sealed = nonce + kNonce;
  const auto sealed_length = bytes.size() - kNonce - kTag;
  unsigned char* tag = bytes.data() + kNonce + sealed_length;

  std::string flow_id(sealed_length, '\0');

  auto context = make_context();
  int length = 0;

  if (EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, _key.data(), nonce) != 1 ||
      EVP_DecryptUpdate(context.get(), reinterpret_cast<unsigned char*>(flow_id.data()), &length, sealed, static_cast<int>(sealed_length)) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTag), tag) != 1) {
    return "";
  }

  // The tag is checked here. Anything that fails it - altered, or sealed under another
  // key - is not a token of ours, and what it decrypted to is thrown away unread.
  if (EVP_DecryptFinal_ex(context.get(), reinterpret_cast<unsigned char*>(flow_id.data()) + length, &length) != 1) return "";

  return flow_id;
}

}  // namespace athenasip
