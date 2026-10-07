//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <unistd.h>

#include <atomic>
#include <boost/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "global_io_context.h"
#include "plugins/plugin.h"
#include "push/jwt.h"
#include "push/push_service.h"

// Keys made fresh for each test, so none is committed, and the independent verification
// the push tests check a driver's tokens with.
namespace push_test {

using Key = std::shared_ptr<EVP_PKEY>;

inline Key generate_ec(const char* curve = "P-256") { return Key(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", curve), EVP_PKEY_free); }

inline Key generate_rsa(unsigned bits = 2048) { return Key(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<size_t>(bits)), EVP_PKEY_free); }

inline std::string private_pem(const Key& key) {
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
  PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr);
  char* data = nullptr;
  const long length = BIO_get_mem_data(bio.get(), &data);
  return std::string(data, static_cast<std::size_t>(length));
}

// A file under the system temporary directory, removed when the object goes.
class TempFile {
 public:
  explicit TempFile(const std::string& contents) {
    static std::atomic<int> counter{0};
    _path = (std::filesystem::temp_directory_path() / ("athenasip-push-" + std::to_string(::getpid()) + "-" + std::to_string(counter++))).string();
    std::ofstream(_path, std::ios::binary) << contents;
  }
  ~TempFile() {
    std::error_code ignored;
    std::filesystem::remove(_path, ignored);
  }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  const std::string& path() const { return _path; }

 private:
  std::string _path;
};

inline std::string uncompressed_point(const Key& key) {
  unsigned char point[65] = {};
  std::size_t length = 0;
  EVP_PKEY_get_octet_string_param(key.get(), OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point), &length);
  return std::string(reinterpret_cast<const char*>(point), length);
}

// A P-256 public key from its X9.62 uncompressed point.
inline Key ec_public_key(const std::string& point) {
  std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)> build(OSSL_PARAM_BLD_new(), OSSL_PARAM_BLD_free);
  OSSL_PARAM_BLD_push_utf8_string(build.get(), OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
  OSSL_PARAM_BLD_push_octet_string(build.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size());
  std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)> params(OSSL_PARAM_BLD_to_param(build.get()), OSSL_PARAM_free);

  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr), EVP_PKEY_CTX_free);
  EVP_PKEY* key = nullptr;
  if (EVP_PKEY_fromdata_init(context.get()) != 1 || EVP_PKEY_fromdata(context.get(), &key, EVP_PKEY_PUBLIC_KEY, params.get()) != 1) return nullptr;
  return Key(key, EVP_PKEY_free);
}

inline bool digest_verify(const Key& key, const std::string& input, const std::string& signature) {
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, key.get()) != 1) return false;
  return EVP_DigestVerify(context.get(), reinterpret_cast<const unsigned char*>(signature.data()), signature.size(),
                          reinterpret_cast<const unsigned char*>(input.data()), input.size()) == 1;
}

struct Token {
  boost::json::object header;
  boost::json::object claims;
  std::string signing_input;
  std::string signature;  // decoded
};

// RFC 7515 7.1: three base64url parts, the first two JSON.
inline std::optional<Token> split(const std::string& compact) {
  const auto first = compact.find('.');
  const auto second = compact.find('.', first == std::string::npos ? 0 : first + 1);
  if (first == std::string::npos || second == std::string::npos || compact.find('.', second + 1) != std::string::npos) return std::nullopt;

  const auto header = athenasip::push::jwt::base64url_decode(compact.substr(0, first));
  const auto claims = athenasip::push::jwt::base64url_decode(compact.substr(first + 1, second - first - 1));
  const auto signature = athenasip::push::jwt::base64url_decode(compact.substr(second + 1));
  if (!header || !claims || !signature) return std::nullopt;

  try {
    Token token;
    token.header = boost::json::parse(*header).as_object();
    token.claims = boost::json::parse(*claims).as_object();
    token.signing_input = compact.substr(0, second);
    token.signature = *signature;
    return token;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

// RFC 7518 3.4: R || S back into the DER OpenSSL verifies.
inline bool verify_es256(const Token& token, const Key& key) {
  if (token.signature.size() != 64) return false;

  ECDSA_SIG* sig = ECDSA_SIG_new();
  BIGNUM* r = BN_bin2bn(reinterpret_cast<const unsigned char*>(token.signature.data()), 32, nullptr);
  BIGNUM* s = BN_bin2bn(reinterpret_cast<const unsigned char*>(token.signature.data()) + 32, 32, nullptr);
  ECDSA_SIG_set0(sig, r, s);

  unsigned char* der = nullptr;
  const int length = i2d_ECDSA_SIG(sig, &der);
  const std::string encoded(reinterpret_cast<const char*>(der), static_cast<std::size_t>(length));
  OPENSSL_free(der);
  ECDSA_SIG_free(sig);

  return digest_verify(key, token.signing_input, encoded);
}

inline bool verify_rs256(const Token& token, const Key& key) { return digest_verify(key, token.signing_input, token.signature); }

inline std::int64_t now_seconds() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

inline athenasip::plugins::Status send(athenasip::push::PushService& service, athenasip::push::Notification notification) {
  std::promise<athenasip::plugins::Status> answered;
  auto future = answered.get_future();

  service.send(athenasip::detail::get_global_io_context().get_executor(), std::move(notification),
               [&answered](athenasip::plugins::Status status) { answered.set_value(std::move(status)); });

  if (future.wait_for(std::chrono::seconds(15)) != std::future_status::ready) return athenasip::plugins::Status::failure("test: no answer");
  return future.get();
}

}  // namespace push_test
