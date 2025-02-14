//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "session.h"

namespace athenasip {

Session::Session(std::shared_ptr<Logger> logger, std::string nonce_secret) : _logger(logger), _nonce_secret(nonce_secret) {}

void Session::_process_message() {
  _current_message->print();
  switch (state) {
    case State::Initial:
      _process_message_initial();
      break;
    case State::Challenged:
      _process_message_challenged();
    default:
      _logger->error("Unknown State while processing message: " + std::to_string(state));
  }
  _current_message.reset();  // replace with process
  return;
}

void Session::_process_message_initial() {
  if (_current_message->header->request_method == "REGISTER") {
    // Generate Nonce and send 401 Unauthenticated

    auto reply = std::make_shared<SIPMessage>();
    reply->body_length = 0;

    // Set up Header
    auto nonce = _generate_nonce();
    AuthorizationHeader authHeader;

    authHeader.type = "Digest";
    authHeader["realm"] = "sip.athenasip.org";
    authHeader["nonce"] = nonce;
    authHeader["algorithm"] = "MD5";

    reply->header = std::make_shared<SIPHeader>();
    reply->header->type = SIPHeader::Type::Response;
    reply->header->response_code = 401;
    reply->header->response_message = "Unauthorized";
    (*reply->header)["WWW-Authenticate"] = authHeader.to_string();
    (*reply->header)["To"] = (*_current_message->header)["From"];
    (*reply->header)["From"] = "<sip:server@sip.athenasip.org>;tag=123456";
    (*reply->header)["Call-ID"] = (*_current_message->header)["Call-ID"];
    (*reply->header)["CSeq"] = "1 REGISTER";
    (*reply->header)["Via"] = (*_current_message->header)["Via"];
    (*reply->header)["Content-Length"] = "0";

    _logger->info("Initial / REGISTER - Sending 401 Challenge");
    write(reply->to_string());

    state = State::Challenged;
  }
}

void Session::_process_message_challenged() {
  if (_current_message->header->request_method == "REGISTER") {
    auto reply = std::make_shared<SIPMessage>();
    reply->body_length = 0;

    auto incomingAuthHeader = AuthorizationHeader((*_current_message->header)["Authorization"]);

    _logger->debug("Challenged / REGISTER - Checking Auth");

    // Set up Header
    reply->header = std::make_shared<SIPHeader>();
    reply->header->type = SIPHeader::Type::Response;

    _logger->debug("Challenged / REGISTER - Authorized, Sending 200 OK");
    reply->header->response_code = 200;
    reply->header->response_message = "OK";
    (*reply->header)["To"] = (*_current_message->header)["From"];
    (*reply->header)["Contact"] = (*_current_message->header)["Contact"];
    (*reply->header)["From"] = "<sip:server@sip.athenasip.org>;tag=123456";
    (*reply->header)["Call-ID"] = (*_current_message->header)["Call-ID"];
    (*reply->header)["CSeq"] = "1 REGISTER";
    (*reply->header)["Via"] = (*_current_message->header)["Via"];
    (*reply->header)["Content-Length"] = "0";

    reply->print();
    write(reply->to_string());

    state = State::Registered;
  }
}

std::string Session::_generate_nonce() {
  std::array<unsigned char, 16> random_bytes;

  // Generate 128-bit (16-byte) secure random data
  if (RAND_bytes(random_bytes.data(), random_bytes.size()) != 1) {
    throw std::runtime_error("Failed to generate secure random bytes");
  }

  // Get the current UNIX timestamp
  uint64_t timestamp = static_cast<uint64_t>(std::time(nullptr));

  // Concatenate random bytes and timestamp
  std::ostringstream raw_nonce_stream;
  raw_nonce_stream << Util::to_hex(random_bytes.data(), random_bytes.size()) << ":" << timestamp;
  std::string raw_nonce = raw_nonce_stream.str();

  // Compute HMAC-SHA256 using OpenSSL
  unsigned char hmac_result[EVP_MAX_MD_SIZE];
  unsigned int hmac_len = 0;

  HMAC(EVP_sha256(), _nonce_secret.c_str(), _nonce_secret.size(), reinterpret_cast<const unsigned char*>(raw_nonce.c_str()), raw_nonce.size(), hmac_result,
       &hmac_len);

  // Convert HMAC output to hex
  std::string hmac_hex = Util::to_hex(hmac_result, hmac_len);

  // Return final nonce in format: "random:timestamp:hmac"
  return raw_nonce + ":" + hmac_hex;
}

void Session::on_register(EventFn callback) { _on_register = callback; }

void Session::on_unregister(EventFn callback) { _on_unregister = callback; }

}  // namespace athenasip