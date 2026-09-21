//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <optional>
#include <string>
#include <utility>

namespace athenasip::api {

namespace http = boost::beast::http;

// One error shape for the whole API:
//
//   { "error": { "code": "conflict", "message": "realm example.com already exists" } }
//
// The code is for a client to branch on and never changes wording; the message is for a
// person reading a log. A client that has to parse prose to tell a duplicate from a
// typo is a client that breaks on the next release.
inline void write_error(const std::shared_ptr<http::response<http::string_body>>& response, http::status status, std::string code, std::string message) {
  boost::json::object error;
  error["code"] = std::move(code);
  error["message"] = std::move(message);

  boost::json::object body;
  body["error"] = std::move(error);

  response->result(status);
  response->set(http::field::content_type, "application/json");
  response->body() = boost::json::serialize(body);
}

inline void write_json(const std::shared_ptr<http::response<http::string_body>>& response, http::status status, const boost::json::value& value) {
  response->result(status);
  response->set(http::field::content_type, "application/json");
  response->body() = boost::json::serialize(value);
}

// 204 carries no body, so it gets no content type either.
inline void write_no_content(const std::shared_ptr<http::response<http::string_body>>& response) {
  response->result(http::status::no_content);
  response->body().clear();
}

// A body that is not an object is not a request this API understands, and saying so is
// the difference between a 400 a client can act on and a 500 nobody can.
inline std::optional<boost::json::object> parse_object(const std::string& body) {
  if (body.empty()) return boost::json::object{};

  boost::system::error_code ec;
  const auto parsed = boost::json::parse(body, ec);
  if (ec || !parsed.is_object()) return std::nullopt;

  return parsed.as_object();
}

// A string field, absent rather than empty when it is not there: "not given" and "given
// as empty" are different answers on a PUT.
inline std::optional<std::string> string_field(const boost::json::object& object, const std::string& name) {
  const auto it = object.find(name);
  if (it == object.end() || !it->value().is_string()) return std::nullopt;

  return std::string(it->value().as_string());
}

inline std::optional<std::uint32_t> uint_field(const boost::json::object& object, const std::string& name) {
  const auto it = object.find(name);
  if (it == object.end()) return std::nullopt;

  if (it->value().is_int64() && it->value().as_int64() >= 0) return static_cast<std::uint32_t>(it->value().as_int64());
  if (it->value().is_uint64()) return static_cast<std::uint32_t>(it->value().as_uint64());

  return std::nullopt;
}

}  // namespace athenasip::api
