//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "cluster_ca.h"

#include <fcntl.h>
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <unistd.h>

#include <boost/asio/ip/address.hpp>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>

namespace athenasip::ca {

namespace {

namespace fs = std::filesystem;

constexpr long kCaDays = 3650;
constexpr long kNodeDays = 730;

using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using CertPtr = std::unique_ptr<X509, decltype(&X509_free)>;

Result failure(std::string why) {
  Result result;
  result.error = std::move(why);
  return result;
}

KeyPtr new_key() { return {EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256"), EVP_PKEY_free}; }

// RFC 5280 4.1.2.2: a positive serial of up to 20 bytes, unique per CA. 159 random bits
// are unique without the CA keeping state.
bool set_serial(X509* certificate) {
  std::unique_ptr<BIGNUM, decltype(&BN_free)> serial(BN_new(), BN_free);
  if (!serial || BN_rand(serial.get(), 159, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1) return false;
  return BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(certificate)) != nullptr;
}

bool add_extension(X509* certificate, X509* issuer, int nid, const std::string& value) {
  X509V3_CTX context;
  X509V3_set_ctx_nodb(&context);
  X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);

  X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str());
  if (extension == nullptr) return false;

  const bool added = X509_add_ext(certificate, extension, -1) == 1;
  X509_EXTENSION_free(extension);
  return added;
}

CertPtr new_certificate(EVP_PKEY* key, const std::string& common_name, long days) {
  CertPtr certificate(X509_new(), X509_free);
  if (!certificate) return certificate;

  X509_set_version(certificate.get(), 2);
  if (!set_serial(certificate.get())) return {nullptr, X509_free};

  X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60);
  X509_gmtime_adj(X509_getm_notAfter(certificate.get()), days * 24 * 60 * 60);
  X509_set_pubkey(certificate.get(), key);

  X509_NAME* subject = X509_get_subject_name(certificate.get());
  X509_NAME_add_entry_by_txt(subject, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("AthenaSIP"), -1, -1, 0);
  X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(common_name.c_str()), -1, -1, 0);

  return certificate;
}

// O_EXCL: two concurrent runs cannot both create the file, and a key is never readable by
// anyone else.
bool write_pem(const fs::path& file, mode_t mode, const std::function<bool(FILE*)>& write) {
  const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
  if (fd < 0) return false;

  FILE* out = ::fdopen(fd, "w");
  if (out == nullptr) {
    ::close(fd);
    return false;
  }

  const bool written = write(out);
  return std::fclose(out) == 0 && written;
}

bool write_key(const fs::path& file, EVP_PKEY* key) {
  return write_pem(file, 0600, [key](FILE* out) { return PEM_write_PrivateKey(out, key, nullptr, nullptr, 0, nullptr, nullptr) == 1; });
}

bool write_certificate(const fs::path& file, X509* certificate) {
  return write_pem(file, 0644, [certificate](FILE* out) { return PEM_write_X509(out, certificate) == 1; });
}

KeyPtr read_key(const fs::path& file) {
  FILE* in = std::fopen(file.c_str(), "r");
  if (in == nullptr) return {nullptr, EVP_PKEY_free};
  EVP_PKEY* key = PEM_read_PrivateKey(in, nullptr, nullptr, nullptr);
  std::fclose(in);
  return {key, EVP_PKEY_free};
}

CertPtr read_certificate(const fs::path& file) {
  FILE* in = std::fopen(file.c_str(), "r");
  if (in == nullptr) return {nullptr, X509_free};
  X509* certificate = PEM_read_X509(in, nullptr, nullptr, nullptr);
  std::fclose(in);
  return {certificate, X509_free};
}

// A node id names files in the CA directory, so it must never be a path.
bool usable_id(const std::string& id) {
  if (id.empty() || id.front() == '.' || id.size() > 64) return false;
  for (const char c : id) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') return false;
  }
  return true;
}

}  // namespace

Result init(const std::string& dir) {
  const fs::path root(dir);
  const auto key_file = root / "ca.key";
  const auto certificate_file = root / "ca.crt";

  if (fs::exists(key_file) || fs::exists(certificate_file)) {
    return failure("a cluster CA is already in " + dir + "; replacing it would orphan every node certificate it signed");
  }

  std::error_code error;
  fs::create_directories(root, error);
  if (error) return failure("cannot create " + dir + " - " + error.message());
  fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace, error);

  auto key = new_key();
  if (!key) return failure("cannot generate a key");

  auto certificate = new_certificate(key.get(), "AthenaSIP Cluster CA", kCaDays);
  if (!certificate) return failure("cannot make a certificate");

  X509_set_issuer_name(certificate.get(), X509_get_subject_name(certificate.get()));

  // RFC 5280 4.2.1.9, 4.2.1.3: an authority that signs certificates and nothing else.
  if (!add_extension(certificate.get(), certificate.get(), NID_basic_constraints, "critical,CA:TRUE,pathlen:0") ||
      !add_extension(certificate.get(), certificate.get(), NID_key_usage, "critical,keyCertSign,cRLSign") ||
      !add_extension(certificate.get(), certificate.get(), NID_subject_key_identifier, "hash")) {
    return failure("cannot describe the certificate");
  }

  if (X509_sign(certificate.get(), key.get(), EVP_sha256()) == 0) return failure("cannot sign the certificate");

  if (!write_key(key_file, key.get())) return failure("cannot write " + key_file.string());
  if (!write_certificate(certificate_file, certificate.get())) return failure("cannot write " + certificate_file.string());

  Result result;
  result.ok = true;
  result.certificate = certificate_file.string();
  result.key = key_file.string();
  return result;
}

Result issue_node(const std::string& dir, const std::string& node_id, const std::vector<std::string>& names, bool replace) {
  if (!usable_id(node_id)) return failure("'" + node_id + "' is not a node id: letters, digits, '-', '_' and '.', not starting with '.'");

  const fs::path root(dir);
  auto authority_key = read_key(root / "ca.key");
  auto authority = read_certificate(root / "ca.crt");
  if (!authority_key || !authority) return failure("no cluster CA in " + dir + "; make one with --ca-init first");

  const auto key_file = root / (node_id + ".key");
  const auto certificate_file = root / (node_id + ".crt");

  if (fs::exists(key_file) || fs::exists(certificate_file)) {
    if (!replace) return failure("node " + node_id + " already has a certificate in " + dir + "; ask to replace it");

    std::error_code ignored;
    fs::remove(key_file, ignored);
    fs::remove(certificate_file, ignored);
  }

  auto key = new_key();
  if (!key) return failure("cannot generate a key");

  auto certificate = new_certificate(key.get(), node_id, kNodeDays);
  if (!certificate) return failure("cannot make a certificate");

  X509_set_issuer_name(certificate.get(), X509_get_subject_name(authority.get()));

  // SANs (RFC 5280 4.2.1.6): the node id as a DNS name, then each given name as an IP name
  // if it is an address, since a peer verifying an address checks only IP names.
  std::string alt = "DNS:" + node_id;
  for (const auto& name : names) {
    if (name.empty() || name == node_id) continue;
    boost::system::error_code not_address;
    boost::asio::ip::make_address(name, not_address);
    alt += (not_address ? ",DNS:" : ",IP:") + name;
  }

  if (!add_extension(certificate.get(), authority.get(), NID_basic_constraints, "critical,CA:FALSE") ||
      !add_extension(certificate.get(), authority.get(), NID_key_usage, "critical,digitalSignature,keyAgreement") ||
      !add_extension(certificate.get(), authority.get(), NID_ext_key_usage, "serverAuth,clientAuth") ||
      !add_extension(certificate.get(), authority.get(), NID_subject_key_identifier, "hash") ||
      !add_extension(certificate.get(), authority.get(), NID_authority_key_identifier, "keyid:always") ||
      !add_extension(certificate.get(), authority.get(), NID_subject_alt_name, alt)) {
    return failure("cannot describe the certificate");
  }

  if (X509_sign(certificate.get(), authority_key.get(), EVP_sha256()) == 0) return failure("cannot sign the certificate");

  if (!write_key(key_file, key.get())) return failure("cannot write " + key_file.string());
  if (!write_certificate(certificate_file, certificate.get())) return failure("cannot write " + certificate_file.string());

  Result result;
  result.ok = true;
  result.certificate = certificate_file.string();
  result.key = key_file.string();
  return result;
}

}  // namespace athenasip::ca
