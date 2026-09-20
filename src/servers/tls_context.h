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

// One loader for every secure listener the node runs. A node whose TLS listener and
// whose secure WebSocket listener were set up two different ways is a node whose
// security depends on which port you reached it on, and the difference would be
// invisible until someone looked.
//
// The options are the ones that are not worth making configurable: SSLv2, SSLv3, TLS 1.0
// and TLS 1.1 are all broken and a SIP endpoint that can only speak them is not one this
// node should carry. Everything else - which ciphers, which curves - is OpenSSL's own
// default, which tracks the state of the art better than a list written here would.
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

}  // namespace athenasip::servers
