//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/asio/ssl.hpp>
#include <exception>
#include <memory>
#include <string>

#include "../loggers/logger.h"

namespace athenasip::servers {

// The one loader for every secure listener, so TLS and secure WebSocket are set up identically. SSLv2, SSLv3, TLS 1.0
// and TLS 1.1 are refused; ciphers and curves are OpenSSL's defaults.
inline bool load_tls_certificates(const std::shared_ptr<loggers::Logger>& logger, boost::asio::ssl::context& context, const std::string& cert,
                                  const std::string& key) {
  if (cert.empty() || key.empty()) {
    logger->error("TLS needs both a certificate and a key");
    return false;
  }

  try {
    context.set_options(boost::asio::ssl::context::default_workarounds | boost::asio::ssl::context::no_sslv2 | boost::asio::ssl::context::no_sslv3 |
                        boost::asio::ssl::context::no_tlsv1 | boost::asio::ssl::context::no_tlsv1_1 | boost::asio::ssl::context::single_dh_use);

    context.use_certificate_chain_file(cert);
    logger->info("Using Certificate PEM: " + cert);

    context.use_private_key_file(key, boost::asio::ssl::context::pem);
    logger->info("Using Key PEM: " + key);
  } catch (const std::exception& e) {
    logger->error("Exception While Loading Certificates: " + std::string(e.what()));
    return false;
  }

  return true;
}

// Mutual TLS for the inter-node listener and the flows a node opens to its peers: the far end must present a
// certificate signed by the cluster CA or the handshake fails.
inline bool require_peer_certificates(const std::shared_ptr<loggers::Logger>& logger, boost::asio::ssl::context& context, const std::string& ca) {
  if (ca.empty()) {
    logger->error("Mutual TLS needs the cluster CA certificate");
    return false;
  }

  try {
    context.load_verify_file(ca);
    context.set_verify_mode(boost::asio::ssl::verify_peer | boost::asio::ssl::verify_fail_if_no_peer_cert);
    logger->info("Requiring peers signed by: " + ca);
  } catch (const std::exception& e) {
    logger->error("Cannot load the cluster CA " + ca + " - " + e.what());
    return false;
  }

  return true;
}

}  // namespace athenasip::servers
