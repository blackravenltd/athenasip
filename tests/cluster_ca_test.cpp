//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "cluster_ca.h"

#include <gtest/gtest.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <sys/stat.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

using namespace athenasip;

namespace {

struct TempDir {
  std::filesystem::path path;

  TempDir() {
    path = std::filesystem::temp_directory_path() / ("athenasip-ca-" + std::to_string(::getpid()) + "-" + std::to_string(std::rand()));
    std::filesystem::create_directories(path);
  }

  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

using X509Ptr = std::unique_ptr<X509, decltype(&X509_free)>;

X509Ptr read_certificate(const std::filesystem::path& file) {
  FILE* in = std::fopen(file.c_str(), "r");
  if (in == nullptr) return {nullptr, X509_free};
  X509* certificate = PEM_read_X509(in, nullptr, nullptr, nullptr);
  std::fclose(in);
  return {certificate, X509_free};
}

bool verifies_against(X509* certificate, X509* authority) {
  std::unique_ptr<X509_STORE, decltype(&X509_STORE_free)> store(X509_STORE_new(), X509_STORE_free);
  X509_STORE_add_cert(store.get(), authority);

  std::unique_ptr<X509_STORE_CTX, decltype(&X509_STORE_CTX_free)> context(X509_STORE_CTX_new(), X509_STORE_CTX_free);
  X509_STORE_CTX_init(context.get(), store.get(), certificate, nullptr);
  return X509_verify_cert(context.get()) == 1;
}

std::string names_of(X509* certificate) {
  std::string out;
  auto* names = static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr));
  for (int i = 0; names != nullptr && i < sk_GENERAL_NAME_num(names); ++i) {
    const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, i);
    if (name->type == GEN_DNS) out += "DNS:" + std::string(reinterpret_cast<const char*>(ASN1_STRING_get0_data(name->d.dNSName))) + " ";
    if (name->type == GEN_IPADD) {
      const auto* bytes = ASN1_STRING_get0_data(name->d.iPAddress);
      if (ASN1_STRING_length(name->d.iPAddress) == 4) {
        out += "IP:" + std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." + std::to_string(bytes[2]) + "." + std::to_string(bytes[3]) + " ";
      }
    }
  }
  GENERAL_NAMES_free(names);
  return out;
}

}  // namespace

// init writes a self-signed CA certificate and a key readable by its owner only.
TEST(ClusterCaTest, InitMakesACertificateAuthority) {
  TempDir dir;

  const auto made = ca::init(dir.path.string());
  ASSERT_TRUE(made.ok) << made.error;

  const auto certificate = read_certificate(dir.path / "ca.crt");
  ASSERT_NE(certificate, nullptr);
  EXPECT_EQ(X509_check_ca(certificate.get()), 1);
  EXPECT_TRUE(verifies_against(certificate.get(), certificate.get()));

  struct stat key{};
  ASSERT_EQ(::stat((dir.path / "ca.key").c_str(), &key), 0);
  EXPECT_EQ(key.st_mode & 0777, 0600) << "the CA key is readable by its owner only";
}

// A second init is refused: replacing the CA would orphan every node certificate.
TEST(ClusterCaTest, InitNeverReplacesAnAuthority) {
  TempDir dir;
  ASSERT_TRUE(ca::init(dir.path.string()).ok);

  const auto before = std::filesystem::last_write_time(dir.path / "ca.key");
  const auto again = ca::init(dir.path.string());

  EXPECT_FALSE(again.ok);
  EXPECT_NE(again.error.find("already"), std::string::npos) << again.error;
  EXPECT_EQ(std::filesystem::last_write_time(dir.path / "ca.key"), before);
}

// A node certificate is signed by the CA, carries the node id and every SAN, and is valid as both
// TLS server and client.
TEST(ClusterCaTest, ANodeCertificateIsSignedByTheClusterAndNamesTheNode) {
  TempDir dir;
  ASSERT_TRUE(ca::init(dir.path.string()).ok);

  const auto issued = ca::issue_node(dir.path.string(), "node-a", {"10.35.1.20", "node-a.example.com"});
  ASSERT_TRUE(issued.ok) << issued.error;

  const auto authority = read_certificate(dir.path / "ca.crt");
  const auto certificate = read_certificate(dir.path / "node-a.crt");
  ASSERT_NE(certificate, nullptr);

  EXPECT_TRUE(verifies_against(certificate.get(), authority.get()));
  EXPECT_EQ(X509_check_ca(certificate.get()), 0) << "a node is not an authority";

  const auto names = names_of(certificate.get());
  EXPECT_NE(names.find("DNS:node-a "), std::string::npos) << names;
  EXPECT_NE(names.find("DNS:node-a.example.com "), std::string::npos) << names;
  EXPECT_NE(names.find("IP:10.35.1.20 "), std::string::npos) << names;

  EXPECT_EQ(X509_check_purpose(certificate.get(), X509_PURPOSE_SSL_SERVER, 0), 1);
  EXPECT_EQ(X509_check_purpose(certificate.get(), X509_PURPOSE_SSL_CLIENT, 0), 1);

  struct stat key{};
  ASSERT_EQ(::stat((dir.path / "node-a.key").c_str(), &key), 0);
  EXPECT_EQ(key.st_mode & 0777, 0600);
}

// Issuing needs a CA, re-issuing needs --replace, and a node id cannot be a path.
TEST(ClusterCaTest, ANodeCertificateIsReplacedOnlyWhenAskedAndNeedsAnAuthority) {
  TempDir dir;

  EXPECT_FALSE(ca::issue_node(dir.path.string(), "node-a", {}).ok) << "no CA yet";

  ASSERT_TRUE(ca::init(dir.path.string()).ok);
  ASSERT_TRUE(ca::issue_node(dir.path.string(), "node-a", {}).ok);
  EXPECT_FALSE(ca::issue_node(dir.path.string(), "node-a", {}).ok);
  EXPECT_TRUE(ca::issue_node(dir.path.string(), "node-a", {}, true).ok);

  EXPECT_FALSE(ca::issue_node(dir.path.string(), "../escape", {}).ok) << "a node id is a name, not a path";
}
