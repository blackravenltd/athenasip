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

Session::Session(std::shared_ptr<Logger> logger, std::string nonce_secret)
    : _logger(logger), _nonce_secret(nonce_secret), _nonces(std::make_shared<ExpirySet<std::string>>()) {}

void Session::_process_message() {
  // Print Incoming (DEBUG)
  _request->print();

  // Create Response
  _response = std::make_shared<SIPMessage>();
  _response->header = std::make_shared<SIPHeader>();
  _response->header->type = SIPHeader::Type::Response;
  _response->body_length = 0;

  // Preflight, check basic headers
  if (!_request->header->contains("From") || !_request->header->contains("To") || !_request->header->contains("Call-ID") ||
      !_request->header->contains("CSeq") || !_request->header->contains("Via") || !_request->header->contains("Max-Forwards")) {
    _logger->info("[Request] - Incomplete Headers, Sending 400 Bad Request and Closing");
    _send_close(400, "Bad Request");
    return;
  }

  // Add From/To Headers
  _response->header->add("From", std::make_shared<StringHeader>("<sip:server@sip.athenasip.org>;tag=123456"));
  _response->header->add("To", _request->header->headers_map["From"][0]);

  // Copy Request Headers
  _response->header->add("Call-Id", _request->header->headers_map["Call-ID"][0]);
  _response->header->add("CSeq", _request->header->headers_map["CSeq"][0]);
  _response->header->add("Via", _request->header->headers_map["Via"][0]);

  // Process according to state
  switch (state) {
    case State::Initial:
      _process_message_initial();
      break;
    case State::Challenged:
      _process_message_challenged();
      break;
    case State::Registered:
      _process_message_challenged();
      break;
    default:
      _logger->error("Unknown State while processing message: " + std::to_string(state));
  }
  _request.reset();
  _response.reset();
  return;
}

void Session::_process_message_initial() {
  if (_request->header->request_method == "REGISTER") {
    // Generate and save nonce
    auto nonce = _generate_nonce();
    _nonces->add(nonce, 3600);

    auto authHeader = std::make_shared<Authorization>();
    authHeader->type = "Digest";
    authHeader->fields["realm"] = "sip.athenasip.org";
    authHeader->fields["nonce"] = nonce;
    authHeader->fields["algorithm"] = "MD5";
    _response->header->add("WWW-Authenticate", std::make_shared<AuthorizationHeader>(authHeader));

    _logger->info("Initial / REGISTER - Sending 401 Challenge");
    state = State::Challenged;
    _send(401, "Unauthorized");
  }
}

void Session::_process_message_challenged() {
  if (_request->header->request_method == "REGISTER") {
    // The Authorization must have been sent
    if (!_request->header->contains("Authorization")) {
      _logger->info("Challenged / REGISTER - No Authorization Header, Sending 401 Unauthorized and Closing");
      _send_close(401, "Unauthorized");
      return;
    }

    // Process it and the identity
    auto incomingAuthHeader = _request->header->headers_map["Authorization"][0]->as<AuthorizationHeader>()->value;
    auto fromIdentity = _request->header->headers_map["From"][0]->as<SIPIdentityHeader>()->value;

    // Check nonce exists
    auto nonce = incomingAuthHeader->fields["nonce"];
    if (!_nonces->contains(nonce)) {
      _logger->info("Challenged / REGISTER - Nonce not found or expired, Sending 401 Unauthorized and Closing");
      _send_close(401, "Unauthorized");
      return;
    }

    // Get Subscriber
    auto h1 = _on_authenticate(fromIdentity, shared_from_this());
    if (!h1) {
      _logger->info("Challenged / REGISTER - User " + fromIdentity->to_string() + " Not Found, Sending 401 Unauthorized and Closing");
      _send_close(401, "Unauthorized");
      return;
    }

    // Generate H2/H3
    auto h2 = Util::md5("REGISTER:" + incomingAuthHeader->fields["uri"]);
    auto const colon = std::string(":");
    auto h3 = Util::md5(h1.value() + colon + nonce + colon + h2);

    // Check match
    if (h3 != incomingAuthHeader->fields["response"]) {
      _logger->info("Challenged / REGISTER - User " + fromIdentity->to_string() + " Digest hash does not match, Sending 401 Unauthorized and Closing");
      _send_close(401, "Unauthorized");
      return;
    }

    // Auth is good
    _logger->debug("Challenged / REGISTER - Authorized, Sending 200 OK");
    _response->header->add("Contact", _request->header->headers_map["Contact"][0]);

    state = State::Registered;
    _send(200, "OK");
  }
}

void Session::_send_close(uint16_t code, std::string message) {
  _send(code, message);
  close();
}

void Session::_send(uint16_t code, std::string message) {
  _response->header->response_code = code;
  _response->header->response_message = message;
  _response->header->add("Content-Length", std::make_shared<UIntHeader>(_response->body.size()));
  _response->print();
  write(_response->to_string());
  if (_response->body.size() > 0) write(_response->body);
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

void Session::on_start(StartCloseFn callback) { _on_start = callback; }
void Session::on_close(StartCloseFn callback) { _on_close = callback; }
void Session::on_authenticate(AuthenticateFn callback) { _on_authenticate = callback; }

}  // namespace athenasip