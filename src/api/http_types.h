//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <string>

namespace athenasip::api {

class HTTPHeader {
 public:
  std::string name;
  std::string value;
};

class Request {
 public:
  Request();
  std::string method;
  std::vector<std::shared_ptr<HTTPHeader>> headers;
  std::string body;
};

class Response {
 public:
  Response();
  uint16_t code;
  std::string message;
  std::vector<std::shared_ptr<HTTPHeader>> headers;
  std::string body;
};

}  // namespace athenasip::api