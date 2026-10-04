//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "webpush_push_service.h"

#include <openssl/core_names.h>

#include <algorithm>
#include <array>
#include <boost/json.hpp>
#include <cctype>
#include <chrono>

#include "../config.h"

namespace athenasip::push {
namespace {

std::string to_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool starts_with_ignoring_case(const std::string& value, const std::string& prefix) { return to_lower(value.substr(0, prefix.size())) == prefix; }

// RFC 8292 2: well inside the 24 hours a push service will accept, so a clock a little
// ahead of the push service's does not make the token look too long-lived.
constexpr auto kTokenLifetime = std::chrono::hours(12);

}  // namespace

WebpushPushService::WebpushPushService(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url)
    : _base_logger(logger), _logger(std::make_shared<loggers::LoggerScoped>("webpush", std::move(logger))), _url(std::move(url)) {}

std::string WebpushPushService::name() const { return "webpush"; }

std::string WebpushPushService::version() const { return "0.0.1"; }

bool WebpushPushService::configure(const YAML::Node& own_root, const Config& system) {
  (void)system;

  if (!own_root || !own_root.IsMap()) {
    _logger->error("push.webpush needs vapid_private_key and subject");
    return false;
  }

  if (!own_root["vapid_private_key"] || own_root["vapid_private_key"].as<std::string>().empty()) {
    _logger->error("push.webpush.vapid_private_key is required: the PEM P-256 key this server signs VAPID tokens with (RFC 8292)");
    return false;
  }

  std::string error;
  const auto path = own_root["vapid_private_key"].as<std::string>();
  auto key = jwt::read_private_key_file(path, error);
  if (!key) {
    _logger->error("push.webpush.vapid_private_key: " + error);
    return false;
  }

  // RFC 8292 3.2: "k" is the uncompressed point (X9.62), 0x04 || X || Y.
  std::array<unsigned char, 65> point{};
  std::size_t point_length = 0;
  if (EVP_PKEY_get_base_id(key.get()) != EVP_PKEY_EC ||
      EVP_PKEY_get_octet_string_param(key.get(), OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size(), &point_length) != 1 || point_length != 65 ||
      point[0] != 0x04 || jwt::sign(key, "{}", "{}").empty()) {
    _logger->error("push.webpush.vapid_private_key: " + path + " is not a P-256 key (RFC 8292 2 requires ECDSA on P-256)");
    return false;
  }

  _subject = own_root["subject"] ? own_root["subject"].as<std::string>() : std::string();
  const bool mailto = starts_with_ignoring_case(_subject, "mailto:") && _subject.size() > 7;
  const bool https = starts_with_ignoring_case(_subject, "https:") && _subject.size() > 6;
  if (!mailto && !https) {
    _logger->error("push.webpush.subject is required, as a mailto: or https: URI a push service can reach the operator by (RFC 8292 2.1)");
    return false;
  }

  if (own_root["ttl"]) {
    try {
      _ttl = own_root["ttl"].as<long>();
    } catch (const YAML::Exception&) {
      _ttl = -1;
    }

    if (_ttl < 0) {
      _logger->error("push.webpush.ttl is a number of seconds, zero or more (RFC 8030 5.2)");
      return false;
    }
  }

  HttpsClient::Options options;
  if (own_root["ca_file"]) options.ca_file = own_root["ca_file"].as<std::string>();

  auto client = std::make_shared<HttpsClient>(_base_logger, options);
  if (!client->error().empty()) {
    _logger->error("push.webpush.ca_file: " + client->error());
    return false;
  }

  _key = std::move(key);
  _public_key = jwt::base64url_encode(std::string_view(reinterpret_cast<const char*>(point.data()), point.size()));
  _client = std::move(client);

  _logger->info("VAPID public key " + _public_key);
  return true;
}

bool WebpushPushService::health() const { return _client != nullptr; }

std::string WebpushPushService::origin(const std::string& uri) {
  const types::URL url(uri);
  if (!url.is_valid() || to_lower(url.scheme) != "https" || url.host.empty()) return {};

  const auto host = to_lower(url.host);
  std::string serialised = "https://" + (host.find(':') == std::string::npos ? host : "[" + host + "]");

  // RFC 6454 6.1: the port appears only when it is not the scheme's default.
  if (url.port && *url.port != 443) serialised += ":" + std::to_string(*url.port);
  return serialised;
}

bool WebpushPushService::accepts(const Notification& notification) const {
  // RFC 8599 12: pn-param MUST NOT be used, and pn-prid is the push subscription URI.
  return notification.param.empty() && !origin(notification.prid).empty();
}

std::vector<std::pair<std::string, std::string>> WebpushPushService::capabilities() const {
  if (_public_key.empty()) return {};
  return {{"+sip.vapid", _public_key}};
}

void WebpushPushService::send(plugins::Executor on, Notification notification, plugins::StatusHandler handler) {
  if (!_client) return _complete(std::move(on), std::move(handler), plugins::Status::failure("webpush is not configured"));

  if (!accepts(notification)) {
    return _complete(std::move(on), std::move(handler),
                     plugins::Status::failure("webpush needs pn-prid to be an https push subscription URI and no pn-param (RFC 8599 12)"));
  }

  const auto expires = std::chrono::system_clock::now() + kTokenLifetime;

  boost::json::object header;
  header["typ"] = "JWT";
  header["alg"] = "ES256";

  boost::json::object claims;
  claims["aud"] = origin(notification.prid);
  claims["exp"] = std::chrono::duration_cast<std::chrono::seconds>(expires.time_since_epoch()).count();
  claims["sub"] = _subject;

  const auto token = jwt::sign(_key, boost::json::serialize(header), boost::json::serialize(claims));
  if (token.empty()) return _complete(std::move(on), std::move(handler), plugins::Status::failure("webpush could not sign the VAPID token"));

  // RFC 8030 5.2 requires TTL; 5.3 Urgency "high" is for an incoming call, which is what a
  // SIP push is for.
  HttpsHeaders headers = {
      {"TTL", std::to_string(_ttl)},
      {"Urgency", "high"},
      {"Authorization", "vapid t=" + token + ", k=" + _public_key},
  };

  _client->post(on, notification.prid, std::move(headers), {}, [handler = std::move(handler), logger = _logger](plugins::Result<HttpsResponse> result) {
    if (!handler) return;

    if (!result) {
      logger->warn("push failed: " + result.error);
      return handler(plugins::Status::failure("webpush: " + result.error));
    }

    // RFC 8030 5: the push service answers 201 Created once it has accepted the message.
    if (result.value.status == 201) return handler(plugins::Status::success());

    logger->warn("push service answered " + std::to_string(result.value.status));
    handler(plugins::Status::failure("webpush: push service answered " + std::to_string(result.value.status)));
  });
}

}  // namespace athenasip::push
