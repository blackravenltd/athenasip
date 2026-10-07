//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "fcm_push_service.h"

#include <algorithm>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/json.hpp>
#include <cctype>
#include <fstream>
#include <sstream>

#include "../config.h"
#include "../global_io_context.h"

namespace athenasip::push {
namespace {

constexpr char kScope[] = "https://www.googleapis.com/auth/firebase.messaging";
constexpr char kGrantType[] = "urn:ietf:params:oauth:grant-type:jwt-bearer";

// Google issues assertions for an hour at most.
constexpr auto kAssertionLifetime = std::chrono::seconds(3600);

// A token is replaced this long before Google says it expires, so one is never sent as
// it lapses.
constexpr auto kRefreshMargin = std::chrono::seconds(60);

bool is_https_url(const std::string& value) {
  const types::URL url(value);
  if (!url.is_valid() || url.host.empty()) return false;

  std::string scheme = url.scheme;
  std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return scheme == "https";
}

// application/x-www-form-urlencoded: everything but the unreserved characters escaped.
std::string form_encode(const std::string& value) {
  static constexpr char hex[] = "0123456789ABCDEF";

  std::string out;
  for (const unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

// A project ID goes into the request path: Google's are lowercase letters, digits and
// hyphens, with a domain and a colon before them for the older domain-scoped kind.
bool is_project_id(const std::string& value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '.' || c == ':'; });
}

std::string string_field(const boost::json::object& object, const char* name) {
  const auto* found = object.if_contains(name);
  return found && found->is_string() ? std::string(found->as_string()) : std::string();
}

std::int64_t seconds_since_epoch(std::chrono::system_clock::time_point when) {
  return std::chrono::duration_cast<std::chrono::seconds>(when.time_since_epoch()).count();
}

}  // namespace

FcmPushService::FcmPushService(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url)
    : _base_logger(logger),
      _logger(std::make_shared<loggers::LoggerScoped>("fcm", std::move(logger))),
      _url(std::move(url)),
      _strand(boost::asio::make_strand(detail::get_global_io_context())) {}

std::string FcmPushService::name() const { return "fcm"; }

plugins::Settings FcmPushService::settings() {
  using namespace plugins::define;
  return {
      section("fcm", "Firebase Cloud Messaging, for fcm:// in push.urls."),
      required(text("fcm.service_account", "", "The service-account JSON file the Firebase console gives you.")),
      integer("fcm.ttl", "60", "Seconds FCM keeps trying to deliver the push.", 0),
      text("fcm.api_base", "https://fcm.googleapis.com", "Only to go through a proxy."),
      text("fcm.ca_file", "", "More CA certificates to trust, beside the system's."),
  };
}

std::string FcmPushService::version() const { return "0.0.1"; }

bool FcmPushService::configure(const YAML::Node& own_root, const Config& system) {
  (void)system;

  if (!own_root || !own_root.IsMap() || !own_root["service_account"] || own_root["service_account"].as<std::string>().empty()) {
    _logger->error("push.fcm.service_account is required: the path to the service-account key file Google issues for the Firebase project");
    return false;
  }

  const auto path = own_root["service_account"].as<std::string>();
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    _logger->error("push.fcm.service_account: cannot read " + path);
    return false;
  }

  std::stringstream contents;
  contents << file.rdbuf();

  boost::system::error_code ec;
  const auto document = boost::json::parse(contents.str(), ec);
  if (ec || !document.is_object()) {
    _logger->error("push.fcm.service_account: " + path + " is not a JSON object");
    return false;
  }

  const auto& account = document.as_object();
  const auto client_email = string_field(account, "client_email");
  const auto private_key = string_field(account, "private_key");
  const auto token_uri = string_field(account, "token_uri");

  if (client_email.empty() || private_key.empty() || token_uri.empty()) {
    _logger->error("push.fcm.service_account: " + path + " needs client_email, private_key and token_uri");
    return false;
  }

  if (!is_https_url(token_uri)) {
    _logger->error("push.fcm.service_account: token_uri is not an https URL: " + token_uri);
    return false;
  }

  auto key = jwt::read_private_key(private_key);
  if (!key || EVP_PKEY_get_base_id(key.get()) != EVP_PKEY_RSA) {
    _logger->error("push.fcm.service_account: private_key is not an RSA key in PEM");
    return false;
  }

  if (own_root["api_base"]) {
    auto api_base = own_root["api_base"].as<std::string>();
    while (!api_base.empty() && api_base.back() == '/') api_base.pop_back();

    if (!is_https_url(api_base)) {
      _logger->error("push.fcm.api_base is not an https URL: " + api_base);
      return false;
    }
    _api_base = std::move(api_base);
  }

  if (own_root["ttl"]) {
    try {
      _ttl = own_root["ttl"].as<long>();
    } catch (const YAML::Exception&) {
      _ttl = -1;
    }

    if (_ttl < 0) {
      _logger->error("push.fcm.ttl is a number of seconds, zero or more");
      return false;
    }
  }

  HttpsClient::Options options;
  if (own_root["ca_file"]) options.ca_file = own_root["ca_file"].as<std::string>();

  auto client = std::make_shared<HttpsClient>(_base_logger, options);
  if (!client->error().empty()) {
    _logger->error("push.fcm.ca_file: " + client->error());
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(_mutex);
    _access_token.clear();
  }

  _key = std::move(key);
  _key_id = string_field(account, "private_key_id");
  _client_email = client_email;
  _token_uri = token_uri;
  _client = std::move(client);

  const auto project = string_field(account, "project_id");
  _logger->info("service account " + _client_email + (project.empty() ? "" : " of " + project));
  return true;
}

bool FcmPushService::health() const { return _client != nullptr; }

bool FcmPushService::accepts(const Notification& notification) const {
  // RFC 8599 11: pn-param is the project ID, pn-prid the registration token.
  return is_project_id(notification.param) && !notification.prid.empty();
}

void FcmPushService::send(plugins::Executor on, Notification notification, plugins::StatusHandler handler) {
  if (!_client) return _complete(std::move(on), std::move(handler), plugins::Status::failure("fcm is not configured"));

  if (!accepts(notification)) {
    return _complete(std::move(on), std::move(handler),
                     plugins::Status::failure("fcm needs pn-param to be the Firebase project ID and pn-prid the registration token (RFC 8599 11)"));
  }

  auto self = shared_from_this();
  with_access_token([self, on, notification = std::move(notification), handler = std::move(handler)](plugins::Result<std::string> token) mutable {
    if (!token) return _complete(std::move(on), std::move(handler), plugins::Status::failure("fcm: " + token.error));
    self->deliver(std::move(on), notification, token.value, std::move(handler));
  });
}

void FcmPushService::with_access_token(TokenHandler handler) {
  std::unique_lock<std::mutex> lock(_mutex);

  if (!_access_token.empty() && std::chrono::steady_clock::now() < _refresh_at) {
    auto token = _access_token;
    lock.unlock();
    return handler(plugins::Result<std::string>::success(std::move(token)));
  }

  // Pushes that arrive while a token is on its way wait for that one rather than each
  // asking for their own.
  _waiting.push_back(std::move(handler));
  if (_fetching) return;
  _fetching = true;
  lock.unlock();

  fetch_access_token();
}

void FcmPushService::fetch_access_token() {
  const auto now = std::chrono::system_clock::now();

  // RFC 7523 2.1 and 3, as Google's OAuth 2.0 for service accounts profiles them.
  boost::json::object header;
  header["alg"] = "RS256";
  header["typ"] = "JWT";
  if (!_key_id.empty()) header["kid"] = _key_id;

  boost::json::object claims;
  claims["iss"] = _client_email;
  claims["scope"] = kScope;
  claims["aud"] = _token_uri;
  claims["iat"] = seconds_since_epoch(now);
  claims["exp"] = seconds_since_epoch(now + kAssertionLifetime);

  const auto assertion = jwt::sign(_key, boost::json::serialize(header), boost::json::serialize(claims));
  if (assertion.empty()) return token_arrived(plugins::Result<std::string>::failure("could not sign the token request"), {});

  HttpsHeaders headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
  std::string body = "grant_type=" + form_encode(kGrantType) + "&assertion=" + form_encode(assertion);

  auto self = shared_from_this();
  _client->post(_strand, _token_uri, std::move(headers), std::move(body), [self](plugins::Result<HttpsResponse> result) {
    if (!result) return self->token_arrived(plugins::Result<std::string>::failure("token request failed: " + result.error), {});

    boost::system::error_code ec;
    const auto document = boost::json::parse(result.value.body, ec);
    const auto* object = !ec && document.is_object() ? &document.as_object() : nullptr;

    if (result.value.status != 200 || !object) {
      std::string reason = "token endpoint answered " + std::to_string(result.value.status);
      if (object && !string_field(*object, "error").empty()) reason += " " + string_field(*object, "error");
      return self->token_arrived(plugins::Result<std::string>::failure(reason), {});
    }

    const auto token = string_field(*object, "access_token");
    if (token.empty()) return self->token_arrived(plugins::Result<std::string>::failure("token endpoint answered without an access_token"), {});

    std::chrono::seconds lifetime{0};
    if (const auto* expires_in = object->if_contains("expires_in"); expires_in && expires_in->is_number()) {
      lifetime = std::chrono::seconds(expires_in->to_number<std::int64_t>());
    }

    self->token_arrived(plugins::Result<std::string>::success(token), lifetime);
  });
}

void FcmPushService::token_arrived(plugins::Result<std::string> token, std::chrono::seconds lifetime) {
  if (!token) _logger->warn(token.error);

  std::vector<TokenHandler> waiting;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    _fetching = false;
    waiting.swap(_waiting);

    // One that would lapse within the margin serves the pushes already waiting and no more.
    if (token && lifetime > kRefreshMargin) {
      _access_token = token.value;
      _refresh_at = std::chrono::steady_clock::now() + lifetime - kRefreshMargin;
    } else {
      _access_token.clear();
    }
  }

  for (auto& handler : waiting) handler(token);
}

void FcmPushService::forget_access_token(const std::string& token) {
  std::lock_guard<std::mutex> lock(_mutex);
  if (_access_token == token) _access_token.clear();
}

void FcmPushService::deliver(plugins::Executor on, const Notification& notification, const std::string& token, plugins::StatusHandler handler) {
  boost::json::object android;
  android["priority"] = "high";
  android["ttl"] = std::to_string(_ttl) + "s";

  boost::json::object data;
  data["reason"] = notification.reason == Notification::Reason::Refresh ? "refresh" : "call";

  boost::json::object message;
  message["token"] = notification.prid;
  message["android"] = std::move(android);
  message["data"] = std::move(data);

  boost::json::object body;
  body["message"] = std::move(message);

  HttpsHeaders headers = {
      {"Authorization", "Bearer " + token},
      {"Content-Type", "application/json; charset=UTF-8"},
  };

  const auto url = _api_base + "/v1/projects/" + notification.param + "/messages:send";

  auto self = shared_from_this();
  _client->post(std::move(on), url, std::move(headers), boost::json::serialize(body),
                [self, token, handler = std::move(handler)](plugins::Result<HttpsResponse> result) {
                  if (!handler) return;

                  if (!result) {
                    self->_logger->warn("push failed: " + result.error);
                    return handler(plugins::Status::failure("fcm: " + result.error));
                  }

                  if (result.value.status == 200) return handler(plugins::Status::success());

                  // A token Google no longer honours is not used again; the next push asks for another.
                  if (result.value.status == 401) self->forget_access_token(token);

                  self->_logger->warn("FCM answered " + std::to_string(result.value.status));
                  handler(plugins::Status::failure("fcm: FCM answered " + std::to_string(result.value.status)));
                });
}

}  // namespace athenasip::push
