//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "util.h"

namespace athenasip {

std::string Util::to_hex(const std::vector<uint8_t>& vec) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (const auto& num : vec) {
    oss << std::setw(2) << static_cast<unsigned int>(num);
  }
  return oss.str();
}

std::string Util::to_hex(const uint8_t arr[], uint16_t len) {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (uint16_t i = 0; i < len; i++) {
    // Cast to unsigned int to ensure numeric interpretation
    oss << std::setw(2) << static_cast<uint>(arr[i]);
  }
  return oss.str();
}

// Trim leading and trailing whitespace
std::string Util::trim(const std::string& str) { return trim(str, " \t\r\n"); }

std::string Util::trim(const std::string& str, const std::string& trimmable) {
  size_t first = str.find_first_not_of(trimmable);
  size_t last = str.find_last_not_of(trimmable);
  return (first == std::string::npos) ? "" : str.substr(first, last - first + 1);
}

#include <openssl/evp.h>

#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

std::string Util::md5(const std::string& input) {
  // Buffer to hold the digest. EVP_MAX_MD_SIZE is guaranteed to be large enough.
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int digest_len = 0;

  // Create a new digest context.
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");

  // Initialize the digest context for MD5.
  if (EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) != 1) {
    EVP_MD_CTX_free(ctx);
    throw std::runtime_error("EVP_DigestInit_ex failed");
  }

  // Update the digest with the input data.
  if (EVP_DigestUpdate(ctx, input.data(), input.size()) != 1) {
    EVP_MD_CTX_free(ctx);
    throw std::runtime_error("EVP_DigestUpdate failed");
  }

  // Finalize the digest and get the result.
  if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
    EVP_MD_CTX_free(ctx);
    throw std::runtime_error("EVP_DigestFinal_ex failed");
  }

  // Clean up the digest context.
  EVP_MD_CTX_free(ctx);

  // Convert the binary digest to a hexadecimal string.
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < digest_len; ++i) {
    oss << std::setw(2) << static_cast<unsigned int>(digest[i]);
  }
  return oss.str();
}

}  // namespace athenasip