/*
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
*/
#include "sip_message.h"

namespace athenasip {

// Default constructor
SIPMessage::SIPMessage() {}

std::string SIPMessage::to_string() const {
  std::string out;
  out += header->to_string();
  out += "\r\n";
  if (body.size() != 0) out += body;
  return out;
}

void SIPMessage::print() const { std::cout << Util::trim(to_string()) << std::endl; }

std::shared_ptr<SIPMessage> SIPMessage::generate_response() { 
    // Create Response
    auto response = std::make_shared<SIPMessage>();
    response->header = std::make_shared<SIPHeader>();
    response->header->type = SIPHeader::Type::Response;
    response->body_length = 0;

    // Fields
    response->session = session;
    response->callID = callID;
    response->contact = contact;
  
    // Add From/To Headers
    // TODO: Generate proper tag
    response->header->add("From", std::make_shared<StringHeader>("<sip:server@sip.athenasip.org>;tag=123456"));
    response->header->add("To", header->headers_map["From"][0]);
  
    // Copy Request Headers
    response->header->add("Call-ID", header->headers_map["Call-ID"][0]);
    response->header->add("CSeq", header->headers_map["CSeq"][0]);
    response->header->add("Via", header->headers_map["Via"][0]);
  
    // Tell the client what is allowed
    response->header->add("Allow", std::make_shared<StringHeader>("INVITE, ACK, CANCEL, OPTIONS, BYE, REFER, NOTIFY, MESSAGE, INFO"));

    return response;
 }

std::string operator+(const SIPMessage& message, const std::string& str) { return message.to_string() + str; }
std::string operator+(const std::string& str, const SIPMessage& message) { return str + message.to_string(); }

}  // namespace athenasip
