//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "session.h"

using namespace athenasip::types;
using namespace athenasip::headers;

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
      break;
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
    auto authHeader = std::make_shared<Authorization>();

    authHeader->type = "Digest";
    authHeader->fields["realm"] = "sip.athenasip.org";
    authHeader->fields["nonce"] = nonce;
    authHeader->fields["algorithm"] = "MD5";

    reply->header = std::make_shared<SIPHeader>();
    reply->header->type = SIPHeader::Type::Response;
    reply->header->response_code = 401;
    reply->header->response_message = "Unauthorized";
    reply->header->add("WWW-Authenticate", std::make_shared<AuthorizationHeader>(authHeader));
    reply->header->add("To", std::make_shared<StringHeader>(_current_message->header->headers_map["From"][0]->to_string()));
    reply->header->add("From", std::make_shared<StringHeader>("<sip:server@sip.athenasip.org>;tag=123456"));
    reply->header->add("Call-Id", std::make_shared<StringHeader>(_current_message->header->headers_map["Call-ID"][0]->to_string()));
    reply->header->add("CSeq", std::make_shared<CSeqHeader>(_current_message->header->headers_map["CSeq"][0]->to_string()));
    reply->header->add("Via", std::make_shared<StringHeader>(_current_message->header->headers_map["Via"][0]->to_string()));
    reply->header->add("Content-Length", std::make_shared<UIntHeader>(0));

    _logger->info("Initial / REGISTER - Sending 401 Challenge");
    reply->print();
    write(reply->to_string());

    state = State::Challenged;
  }
}

void Session::_process_message_challenged() {
  if (_current_message->header->request_method == "REGISTER") {
    auto reply = std::make_shared<SIPMessage>();
    reply->header = std::make_shared<SIPHeader>();
    reply->header->type = SIPHeader::Type::Response;
    reply->body_length = 0;

    if (!_current_message->header->contains("Authorization")) {
      _logger->debug("Challenged / REGISTER Got Authorization Header ");
      reply->header->response_code = 401;
      reply->header->response_message = "Unauthorized";
      reply->header->add("To", std::make_shared<StringHeader>(_current_message->header->headers_map["From"][0]->to_string()));
      reply->header->add("From", std::make_shared<StringHeader>("<sip:server@sip.athenasip.org>;tag=123456"));
      reply->header->add("Call-Id", std::make_shared<StringHeader>(_current_message->header->headers_map["Call-ID"][0]->to_string()));
      reply->header->add("CSeq", std::make_shared<CSeqHeader>(_current_message->header->headers_map["CSeq"][0]->to_string()));
      reply->header->add("Via", std::make_shared<StringHeader>(_current_message->header->headers_map["Via"][0]->to_string()));
      reply->header->add("Content-Length", std::make_shared<UIntHeader>(0));

      _logger->info("Challenged / REGISTER - Did not receive Authorization Header, Sending 401 Reject and Closing");
      reply->print();
      write(reply->to_string());

      close();
      return;
    }

    auto incomingAuthHeader = _current_message->header->headers_map["Authorization"][0]->as<AuthorizationHeader>()->value;
    _logger->debug("Challenged / REGISTER - Checking Auth");

    // Set up Header
    reply->header = std::make_shared<SIPHeader>();
    reply->header->type = SIPHeader::Type::Response;

    _logger->debug("Challenged / REGISTER - Authorized, Sending 200 OK");
    reply->header->response_code = 200;
    reply->header->response_message = "OK";

    reply->header->add("To", std::make_shared<StringHeader>(_current_message->header->headers_map["From"][0]->to_string()));
    reply->header->add("From", std::make_shared<StringHeader>("<sip:server@sip.athenasip.org>;tag=123456"));
    reply->header->add("Contact", std::make_shared<StringHeader>(_current_message->header->headers_map["Contact"][0]->to_string()));
    reply->header->add("Call-ID", std::make_shared<StringHeader>(_current_message->header->headers_map["Call-ID"][0]->to_string()));
    reply->header->add("CSeq", std::make_shared<CSeqHeader>(_current_message->header->headers_map["CSeq"][0]->to_string()));
    reply->header->add("Via", std::make_shared<StringHeader>(_current_message->header->headers_map["Via"][0]->to_string()));
    reply->header->add("Content-Length", std::make_shared<UIntHeader>(0));

    reply->print();
    write(reply->to_string());

    state = State::Registered;
  }
}

std::string Session::_generate_nonce() const {
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