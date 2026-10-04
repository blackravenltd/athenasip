//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "apns_push_service.h"

#include <algorithm>
#include <boost/json.hpp>
#include <cctype>
#include <optional>

#include "../config.h"

namespace athenasip::push {
namespace {

constexpr char kProduction[] = "https://api.push.apple.com";
constexpr char kSandbox[] = "https://api.sandbox.push.apple.com";

// Apple refuses a provider token older than an hour, and one replaced more often than every
// twenty minutes (TooManyProviderTokenUpdates). Forty minutes sits between the two.
constexpr auto kTokenReuse = std::chrono::minutes(40);
constexpr auto kTokenMinimumAge = std::chrono::minutes(20);

bool is_hex(const std::string& value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c); });
}

bool is_alphanumeric(const std::string& value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isalnum(c); });
}

// A bundle ID with any service suffix: letters, digits, hyphens and periods (and the
// underscore some older ones carry), with no empty label.
bool is_topic(const std::string& value) {
  if (value.empty() || value.front() == '.' || value.back() == '.' || value.find("..") != std::string::npos) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '.' || c == '_'; });
}

bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_https_url(const std::string& value) {
  const types::URL url(value);
  if (!url.is_valid() || url.host.empty()) return false;

  std::string scheme = url.scheme;
  std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return scheme == "https";
}

// A scalar's text; nullopt when the key is absent or not a scalar.
std::optional<std::string> scalar(const YAML::Node& root, const char* key) {
  const auto node = root[key];
  if (!node || !node.IsScalar()) return std::nullopt;
  return node.as<std::string>();
}

std::int64_t seconds_since_epoch(std::chrono::system_clock::time_point when) {
  return std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()).count();
}

}  // namespace

ApnsPushService::ApnsPushService(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url)
    : _base_logger(logger), _logger(std::make_shared<loggers::LoggerScoped>("apns", std::move(logger))), _url(std::move(url)) {}

std::string ApnsPushService::name() const { return "apns"; }

std::string ApnsPushService::version() const { return "0.0.1"; }

bool ApnsPushService::configure(const YAML::Node& own_root, const Config& system) {
  (void)system;

  if (!own_root || !own_root.IsMap()) {
    _logger->error("push.apns needs key_file, key_id and team_id: the APNs key Apple issues, its key ID, and the team's ID");
    return false;
  }

  const auto key_file = scalar(own_root, "key_file");
  if (!key_file || key_file->empty()) {
    _logger->error("push.apns.key_file is required: the path to the .p8 APNs key Apple issues");
    return false;
  }

  std::string error;
  auto key = jwt::read_private_key_file(*key_file, error);
  if (!key) {
    _logger->error("push.apns.key_file: " + error);
    return false;
  }

  // jwt::sign makes ES256 only from a P-256 key, which is what Apple issues.
  if (EVP_PKEY_get_base_id(key.get()) != EVP_PKEY_EC || jwt::sign(key, "{}", "{}").empty()) {
    _logger->error("push.apns.key_file: " + *key_file + " is not a P-256 key");
    return false;
  }

  const auto key_id = scalar(own_root, "key_id");
  if (!key_id || !is_alphanumeric(*key_id)) {
    _logger->error("push.apns.key_id is required: the ten-character ID Apple shows beside the key");
    return false;
  }

  const auto team_id = scalar(own_root, "team_id");
  if (!team_id || !is_alphanumeric(*team_id)) {
    _logger->error("push.apns.team_id is required: the ten-character ID of the Apple developer team");
    return false;
  }

  std::string api_base = kProduction;
  if (own_root["environment"]) {
    const auto environment = scalar(own_root, "environment");
    if (environment == "sandbox") {
      api_base = kSandbox;
    } else if (environment != "production") {
      _logger->error("push.apns.environment is production or sandbox");
      return false;
    }
  }

  if (own_root["api_base"]) {
    auto base = scalar(own_root, "api_base").value_or("");
    while (!base.empty() && base.back() == '/') base.pop_back();

    if (!is_https_url(base)) {
      _logger->error("push.apns.api_base is not an https URL: " + base);
      return false;
    }
    api_base = std::move(base);
  }

  long ttl = 60;
  if (own_root["ttl"]) {
    try {
      ttl = own_root["ttl"].as<long>();
    } catch (const YAML::Exception&) {
      ttl = -1;
    }

    if (ttl < 0) {
      _logger->error("push.apns.ttl is a number of seconds, zero or more");
      return false;
    }
  }

  Http2Client::Options options;
  if (own_root["ca_file"]) options.ca_file = scalar(own_root, "ca_file").value_or("");

  auto client = std::make_shared<Http2Client>(_base_logger, options);
  if (!client->error().empty()) {
    _logger->error("push.apns.ca_file: " + client->error());
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(_mutex);
    _token = {};
  }

  if (_client) _client->close();

  _key = std::move(key);
  _key_id = *key_id;
  _team_id = *team_id;
  _api_base = std::move(api_base);
  _ttl = ttl;
  _client = std::move(client);

  _logger->info("team " + _team_id + ", key " + _key_id + ", " + _api_base);
  return true;
}

bool ApnsPushService::health() const { return _client != nullptr; }

void ApnsPushService::close() {
  if (_client) _client->close();
}

void ApnsPushService::set_clock(Clock clock) { _clock = std::move(clock); }

bool ApnsPushService::accepts(const Notification& notification) const {
  // RFC 8599 10: pn-param is "<Team ID>.<Topic>", split at the first period, and pn-prid
  // the device token, which Apple gives as hex.
  const auto period = notification.param.find('.');
  if (period == std::string::npos || !is_hex(notification.prid)) return false;

  const auto team_id = notification.param.substr(0, period);
  if (!is_alphanumeric(team_id) || !is_topic(notification.param.substr(period + 1))) return false;

  // The node signs as one team, and APNs refuses a topic that belongs to another.
  return _team_id.empty() || team_id == _team_id;
}

void ApnsPushService::send(plugins::Executor on, Notification notification, plugins::StatusHandler handler) {
  if (!_client) return _complete(std::move(on), std::move(handler), plugins::Status::failure("apns is not configured"));

  if (!accepts(notification)) {
    return _complete(std::move(on), std::move(handler),
                     plugins::Status::failure("apns needs pn-param to be " + _team_id + ".<topic> and pn-prid the device token (RFC 8599 10)"));
  }

  const auto topic = notification.param.substr(notification.param.find('.') + 1);
  const bool voip = ends_with(topic, ".voip");
  const auto now = _clock();

  const auto token = provider_token(now);
  if (token.value.empty()) return _complete(std::move(on), std::move(handler), plugins::Status::failure("apns: could not sign the provider token"));

  const char* reason = notification.reason == Notification::Reason::Refresh ? "refresh" : "call";

  boost::json::object body;
  if (!voip) {
    boost::json::object aps;
    aps["content-available"] = 1;
    body["aps"] = std::move(aps);
  }
  body["reason"] = reason;

  // A ttl of 0 is APNs' "deliver now or not at all".
  const auto expiration = _ttl == 0 ? std::string("0") : std::to_string(seconds_since_epoch(now) + _ttl);

  HttpsHeaders headers = {
      {"authorization", "bearer " + token.value},
      {"apns-topic", topic},
      {"apns-push-type", voip ? "voip" : "alert"},
      {"apns-priority", "10"},
      {"apns-expiration", expiration},
      {"content-type", "application/json"},
  };

  auto self = shared_from_this();
  _client->post(std::move(on), _api_base + "/3/device/" + notification.prid, std::move(headers), boost::json::serialize(body),
                [self, token, now, handler = std::move(handler)](plugins::Result<HttpsResponse> result) {
                  if (!handler) return;

                  if (!result) {
                    self->_logger->warn("push failed: " + result.error);
                    return handler(plugins::Status::failure("apns: " + result.error));
                  }

                  if (result.value.status == 200) return handler(plugins::Status::success());

                  // Apple's answer carries why in "reason" (Unregistered, BadDeviceToken, ...).
                  std::string why;
                  boost::system::error_code ec;
                  const auto document = boost::json::parse(result.value.body, ec);
                  if (!ec && document.is_object()) {
                    if (const auto* found = document.as_object().if_contains("reason"); found && found->is_string()) why = found->as_string().c_str();
                  }

                  if (why == "ExpiredProviderToken") self->provider_token_expired(token, now);

                  const auto described = "APNs answered " + std::to_string(result.value.status) + (why.empty() ? "" : " " + why);
                  self->_logger->warn(described);
                  handler(plugins::Status::failure("apns: " + described));
                });
}

ApnsPushService::Token ApnsPushService::provider_token(std::chrono::system_clock::time_point now) {
  std::lock_guard<std::mutex> lock(_mutex);

  // A clock that has gone back would leave the token's iat in the future.
  if (!_token.value.empty() && now >= _token.issued && now - _token.issued < kTokenReuse) return _token;

  // Apple's token-based provider connection trust: ES256 with the key's ID, issued by the
  // team, and nothing else.
  boost::json::object header;
  header["alg"] = "ES256";
  header["kid"] = _key_id;

  boost::json::object claims;
  claims["iss"] = _team_id;
  claims["iat"] = seconds_since_epoch(now);

  _token.value = jwt::sign(_key, boost::json::serialize(header), boost::json::serialize(claims));
  _token.issued = now;
  return _token;
}

void ApnsPushService::provider_token_expired(const Token& token, std::chrono::system_clock::time_point now) {
  std::lock_guard<std::mutex> lock(_mutex);

  // Replacing it any sooner would be TooManyProviderTokenUpdates.
  if (_token.value == token.value && now - token.issued >= kTokenMinimumAge) _token = {};
}

}  // namespace athenasip::push
