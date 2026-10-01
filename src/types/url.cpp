//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "url.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace athenasip::types {

namespace {

// RFC 3986 3.1: scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ).
bool is_scheme(const std::string& value) {
  if (value.empty() || !std::isalpha(static_cast<unsigned char>(value.front()))) return false;

  return std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isalnum(c) || c == '+' || c == '-' || c == '.'; });
}

std::string to_lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

}  // namespace

URL::URL() = default;

URL::URL(const std::string& url) { parse(url); }

void URL::parse(const std::string& url) {
  _valid = false;
  scheme.clear();
  username.reset();
  password.reset();
  host.clear();
  port.reset();
  path.clear();
  query.clear();
  fragment.clear();

  // Hand-written rather than a regex, like the header, URI and identity parsers: the
  // grammar is a sequence of splits on delimiters that cannot appear unescaped in what
  // they delimit, and each split below names the rule it comes from.
  const auto separator = url.find("://");
  if (separator == std::string::npos || !is_scheme(url.substr(0, separator))) return;

  scheme = url.substr(0, separator);

  std::string rest = url.substr(separator + 3);

  // 3986 3.5 then 3.4: the fragment runs to the end of the URL and the query to the
  // fragment, so taking those off first is what leaves the rest unambiguous.
  const auto hash = rest.find('#');
  if (hash != std::string::npos) {
    fragment = rest.substr(hash + 1);
    rest.erase(hash);
  }

  const auto question = rest.find('?');
  if (question != std::string::npos) {
    query = rest.substr(question + 1);
    rest.erase(question);
  }

  // 3986 3.2: the authority ends at the first '/', which is the first character of the
  // path. A URL with no '/' has no path, and "memory://" has neither.
  const auto slash = rest.find('/');
  if (slash != std::string::npos) {
    path = rest.substr(slash);
    rest.erase(slash);
  }

  // 3986 3.2.1: userinfo is everything before the '@', and its own ':' separates the
  // two halves. No ':' means a username and no password, rather than an empty one.
  const auto at = rest.find('@');
  if (at != std::string::npos) {
    const auto userinfo = rest.substr(0, at);
    rest.erase(0, at + 1);

    const auto divider = userinfo.find(':');
    if (divider == std::string::npos) {
      username = userinfo;
    } else {
      username = userinfo.substr(0, divider);
      password = userinfo.substr(divider + 1);
    }
  }

  // 3986 3.2.2: an IPv6 literal is in brackets, and its own colons are why. The host is
  // kept without them, because it is what a driver connects to; to_string puts them back.
  std::string after_host;

  if (!rest.empty() && rest.front() == '[') {
    const auto close = rest.find(']');
    if (close == std::string::npos || close == 1) return;

    host = rest.substr(1, close - 1);
    after_host = rest.substr(close + 1);

    if (after_host.empty()) {
      port = _default_port(scheme);
      _valid = true;
      return;
    }

    if (after_host.front() != ':') return;
  } else {
    const auto colon = rest.find(':');
    if (colon == std::string::npos) {
      host = rest;
      port = _default_port(scheme);
      _valid = true;
      return;
    }

    host = rest.substr(0, colon);
    after_host = rest.substr(colon);
  }

  // 3986 3.2.3: port = *DIGIT. Anything else after the colon is a configuration mistake
  // worth refusing, rather than something to read as part of a hostname and then fail
  // to connect to much later. An unbracketed IPv6 literal ends up here too, with colons
  // in what should be digits, and is refused rather than guessed at.
  const auto digits = after_host.substr(1);
  if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) return;
  if (digits.size() > 5) return;

  const auto value = std::stoul(digits);
  if (value > std::numeric_limits<uint16_t>::max()) return;

  port = static_cast<uint16_t>(value);
  _valid = true;
}

std::optional<uint16_t> URL::_default_port(const std::string& scheme) {
  const auto found = defaultPorts.find(to_lower(scheme));
  return found == defaultPorts.end() ? std::nullopt : std::optional<uint16_t>{found->second};
}

std::string URL::to_string() const {
  std::string url = scheme + "://";
  if (username.has_value()) {
    url += username.value();
    if (password.has_value()) {
      url += ":" + password.value();
    }
    url += "@";
  }
  url += host.find(':') == std::string::npos ? host : "[" + host + "]";

  // The port is left off only when it is the one the scheme implies anyway. A scheme
  // with no well-known port has to carry it, or the port is lost.
  if (port.has_value()) {
    const auto standard = _default_port(scheme);
    if (!standard.has_value() || *standard != *port) url += ":" + std::to_string(*port);
  }
  url += path;
  if (!query.empty()) {
    url += "?" + query;
  }
  if (!fragment.empty()) {
    url += "#" + fragment;
  }
  return url;
}

bool URL::is_valid() const { return _valid; }

bool operator==(const URL& lhs, const URL& rhs) {
  return lhs.scheme == rhs.scheme && lhs.username == rhs.username && lhs.password == rhs.password && lhs.host == rhs.host && lhs.port == rhs.port &&
         lhs.path == rhs.path && lhs.query == rhs.query && lhs.fragment == rhs.fragment;
}

std::string operator+(const URL& url, const std::string& str) { return url.to_string() + str; }
std::string operator+(const std::string& str, const URL& url) { return str + url.to_string(); }

const std::map<std::string, uint16_t> URL::defaultPorts = {{"http", 80},         {"https", 443},       {"ftp", 21},
                                                           {"mysql", 3306},      {"postgresql", 5432}, {"ssh", 22},
                                                           {"telnet", 23},       {"smtp", 25},         {"dns", 53},
                                                           {"http-alt", 8080},   // Alternative HTTP port
                                                           {"https-alt", 8443},  // Alternative HTTPS port
                                                           {"pop3", 110},        {"imap", 143},        {"ldap", 389},
                                                           {"sftp", 22},                          // SFTP shares port with SSH
                                                           {"snmp", 161},        {"smtps", 465},  // SMTP over SSL
                                                           {"imaps", 993},                        // IMAP over SSL
                                                           {"pop3s", 995},                        // POP3 over SSL
                                                           {"redis", 6379},      {"mongodb", 27017},   {"cassandra", 9042},
                                                           {"memcached", 11211}, {"rabbitmq", 5672},   {"mqtt", 1883},
                                                           {"mqtts", 8883},      {"coap", 5683},       {"amqp", 5671},  // AMQP over TLS
                                                           {"rsync", 873},       {"rdp", 3389},        {"elasticsearch", 9200},
                                                           {"kibana", 5601},     {"zookeeper", 2181}};

}  // namespace athenasip::types
