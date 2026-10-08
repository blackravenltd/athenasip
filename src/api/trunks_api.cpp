//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "trunks_api.h"

#include <ctime>
#include <utility>

#include "api/api_json.h"
#include "loggers/logger_scoped.h"
#include "types/sip_uri.h"
#include "types/user.h"
#include "util.h"

namespace athenasip::api {

namespace {

namespace http = boost::beast::http;

const boost::json::value* field(const boost::json::object& body, const char* name) {
  const auto it = body.find(name);
  return it == body.end() ? nullptr : &it->value();
}

}  // namespace

TrunksAPI::TrunksAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor)
    : _logger(std::make_shared<loggers::LoggerScoped>("trunks_api", std::move(logger))), _datastore(std::move(datastore)), _executor(std::move(executor)) {}

void TrunksAPI::register_routes(Router& router) {
  using types::roles::manage_trunks;
  auto self = shared_from_this();

  router.add(http::verb::get, "/api/v1/trunks", {manage_trunks}, [self](RouteContext c) { self->_list(std::move(c)); });
  router.add(http::verb::post, "/api/v1/trunks", {manage_trunks}, [self](RouteContext c) { self->_create(std::move(c)); });
  router.add(http::verb::get, "/api/v1/trunks/{trunk}", {manage_trunks}, [self](RouteContext c) { self->_get(std::move(c)); });
  router.add(http::verb::put, "/api/v1/trunks/{trunk}", {manage_trunks}, [self](RouteContext c) { self->_update(std::move(c)); });
  router.add(http::verb::delete_, "/api/v1/trunks/{trunk}", {manage_trunks}, [self](RouteContext c) { self->_delete(std::move(c)); });
}

boost::json::object TrunksAPI::to_json(const types::Trunk& trunk) const {
  auto out = trunk.to_json();
  out.erase("password");
  out["password_set"] = !trunk.password.empty();

  // {node, state, detail, expires_at, at}; null when no node registers to it, or none has said yet.
  const auto report = trunk.register_enabled && _statuses ? _statuses->find(trunk.name) : std::nullopt;
  out["registration"] = report ? boost::json::value(*report) : boost::json::value(nullptr);
  return out;
}

std::string TrunksAPI::apply(const boost::json::object& body, types::Trunk& trunk, bool creating) {
  if (creating) {
    const auto name = string_field(body, "name");
    if (!name || !types::Trunk::valid_name(*name)) return "name is required: letters, digits, '.', '-' and '_', at most 64";
    trunk.name = *name;
  } else if (field(body, "name") != nullptr && string_field(body, "name").value_or("") != trunk.name) {
    return "a trunk's name cannot change; create another";
  }

  if (const auto* uri = field(body, "uri")) {
    if (!uri->is_string()) return "uri is a SIP URI";
    const types::SIPUri parsed{std::string(uri->as_string())};
    const auto scheme = Util::to_lower(parsed.scheme);
    if (!parsed.valid || (scheme != "sip" && scheme != "sips") || parsed.host.empty()) return "uri is a sip: or sips: URI with a host";
    trunk.uri = std::string(uri->as_string());
  } else if (creating) {
    return "uri is required";
  }

  if (const auto* proxy = field(body, "proxy")) {
    if (!proxy->is_string()) return "proxy is a SIP URI, or empty for none";
    const std::string text(proxy->as_string());
    if (!text.empty()) {
      const types::SIPUri parsed{text};
      const auto scheme = Util::to_lower(parsed.scheme);
      if (!parsed.valid || (scheme != "sip" && scheme != "sips") || parsed.host.empty()) return "proxy is a sip: or sips: URI with a host";
    }
    trunk.proxy = text;
  }

  if (const auto username = string_field(body, "username")) trunk.username = *username;
  if (const auto password = string_field(body, "password")) trunk.password = *password;
  if (const auto ca = string_field(body, "tls_ca")) trunk.tls_ca = *ca;

  if (const auto* registration = field(body, "register")) {
    if (!registration->is_object()) return "register is an object: {enabled, expires, contact_user}";
    const auto& r = registration->as_object();
    if (const auto* enabled = field(r, "enabled")) {
      if (!enabled->is_bool()) return "register.enabled is true or false";
      trunk.register_enabled = enabled->as_bool();
    }
    if (field(r, "expires") != nullptr) {
      const auto expires = uint_field(r, "expires");
      if (!expires || *expires < 60 || *expires > 86400) return "register.expires is 60 to 86400 seconds";
      trunk.register_expires = *expires;
    }
    if (const auto contact = string_field(r, "contact_user")) trunk.contact_user = *contact;
  }

  if (const auto* inbound = field(body, "inbound_addresses")) {
    if (!inbound->is_array()) return "inbound_addresses is a list of addresses or CIDR ranges";
    std::vector<std::string> ranges;
    for (const auto& range : inbound->as_array()) {
      if (!range.is_string() || !types::Trunk::valid_range(std::string(range.as_string()))) {
        return "inbound_addresses: " + (range.is_string() ? std::string(range.as_string()) : std::string("a value")) + " is not an address or CIDR range";
      }
      ranges.emplace_back(range.as_string());
    }
    trunk.inbound_addresses = std::move(ranges);
  }

  if (const auto* attributes = field(body, "attributes")) {
    if (!attributes->is_object()) return "attributes is an object";
    trunk.attributes = attributes->as_object();
  }

  if (trunk.register_enabled && trunk.username.empty() && trunk.contact_user.empty()) {
    return "a trunk that registers needs a username or a contact_user to register as";
  }
  return {};
}

void TrunksAPI::_list(RouteContext context) {
  auto self = shared_from_this();
  _datastore->trunk_list(_executor, [self, context](plugins::Result<std::vector<std::shared_ptr<types::Trunk>>> found) mutable {
    if (!found.ok) {
      write_error(context.response, http::status::internal_server_error, "datastore_error", found.error);
      return context.done();
    }

    boost::json::array out;
    for (const auto& trunk : found.value) out.push_back(self->to_json(*trunk));
    write_json(context.response, http::status::ok, out);
    context.done();
  });
}

void TrunksAPI::_create(RouteContext context) {
  const auto body = parse_object(context.body);
  if (!body) {
    write_error(context.response, http::status::bad_request, "invalid_json", "the body is not a JSON object");
    return context.done();
  }

  auto trunk = std::make_shared<types::Trunk>();
  if (const auto refused = apply(*body, *trunk, true); !refused.empty()) {
    write_error(context.response, http::status::bad_request, "invalid_request", refused);
    return context.done();
  }
  trunk->created_at = std::time(nullptr);

  auto self = shared_from_this();
  _datastore->trunk_create(_executor, trunk, [self, context, trunk](plugins::Status status) mutable {
    if (!status.ok) {
      write_error(context.response, http::status::conflict, "conflict", "trunk " + trunk->name + " already exists");
      return context.done();
    }
    write_json(context.response, http::status::created, self->to_json(*trunk));
    context.done();
  });
}

void TrunksAPI::_get(RouteContext context) {
  const auto name = context.parameter("trunk");
  auto self = shared_from_this();
  _datastore->trunk_get(_executor, name, [self, context, name](plugins::Result<std::shared_ptr<types::Trunk>> found) mutable {
    if (!found.ok) {
      write_error(context.response, http::status::internal_server_error, "datastore_error", found.error);
      return context.done();
    }
    if (!found.value) {
      write_error(context.response, http::status::not_found, "not_found", "no trunk " + name);
      return context.done();
    }
    write_json(context.response, http::status::ok, self->to_json(*found.value));
    context.done();
  });
}

void TrunksAPI::_update(RouteContext context) {
  const auto body = parse_object(context.body);
  if (!body) {
    write_error(context.response, http::status::bad_request, "invalid_json", "the body is not a JSON object");
    return context.done();
  }

  const auto name = context.parameter("trunk");
  auto self = shared_from_this();
  _datastore->trunk_get(_executor, name, [self, context, name, body](plugins::Result<std::shared_ptr<types::Trunk>> found) mutable {
    if (!found.ok) {
      write_error(context.response, http::status::internal_server_error, "datastore_error", found.error);
      return context.done();
    }
    if (!found.value) {
      write_error(context.response, http::status::not_found, "not_found", "no trunk " + name);
      return context.done();
    }

    auto trunk = found.value;
    if (const auto refused = apply(*body, *trunk, false); !refused.empty()) {
      write_error(context.response, http::status::bad_request, "invalid_request", refused);
      return context.done();
    }

    self->_datastore->trunk_update(self->_executor, trunk, [self, context, trunk](plugins::Status status) mutable {
      if (!status.ok) {
        write_error(context.response, http::status::not_found, "not_found", "no trunk " + trunk->name);
        return context.done();
      }
      write_json(context.response, http::status::ok, self->to_json(*trunk));
      context.done();
    });
  });
}

void TrunksAPI::_delete(RouteContext context) {
  const auto name = context.parameter("trunk");
  _datastore->trunk_delete(_executor, name, [context, name](plugins::Status status) mutable {
    if (!status.ok) {
      write_error(context.response, http::status::not_found, "not_found", "no trunk " + name);
      return context.done();
    }
    write_no_content(context.response);
    context.done();
  });
}

}  // namespace athenasip::api
