//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "url.h"

namespace athenasip::types {

URL::URL() = default;

URL::URL(const std::string& url) { parse(url); }

void URL::parse(const std::string& url) {
  // Use std::regex to parse the URL
  static const std::regex pattern(R"(^([a-zA-Z][a-zA-Z0-9+.-]*):\/\/(?:([^:\/?#]*)?:?([^@\/?#]*)?@)?([^:\/?#]+)(?::(\d+))?([^?#]*)(?:\?([^#]*))?(?:#(.*))?$)");
  std::smatch matches;

  // Default is invalid
  _valid = false;

  // If the URL matches
  if (std::regex_match(url, matches, pattern)) {
    try {
      scheme = matches[1];

      // User/Password is optional
      username = matches[2].matched ? std::optional<std::string>{matches[2]} : std::nullopt;
      password = matches[3].matched ? std::optional<std::string>{matches[3]} : std::nullopt;
      host = matches[4];

      // Port is also optional
      if (matches[5].matched && !matches[5].str().empty()) {
        int _port = std::stoi(matches[5].str());
        if (_port < 0 || _port > std::numeric_limits<uint16_t>::max()) {
          throw std::out_of_range("The value is out of the range for uint16_t.");
        }
        port = static_cast<uint16_t>(_port);
      } else {
        // Infer port from scheme
        auto it = defaultPorts.find(scheme);
        port = (it != defaultPorts.end()) ? std::optional<uint16_t>{it->second} : std::nullopt;
      }

      path = matches[6].matched ? matches[6].str() : "/";  // Default path is "/"
      query = matches[7].matched ? matches[7].str() : "";
      fragment = matches[8].matched ? matches[8].str() : "";

      // If we reach here, the URL is valid.
      _valid = true;
    } catch (const std::exception&) {
      // Something went wrong. Assume invalid.
    }
  }
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
  url += host;
  // Only add the port if it's not the default port for the scheme
  if (port.has_value()) {
    // Convert the scheme to lowercase to match the keys in defaultPorts map
    std::string schemeLower = scheme;
    std::transform(schemeLower.begin(), schemeLower.end(), schemeLower.begin(), [](unsigned char c) { return std::tolower(c); });

    auto it = defaultPorts.find(schemeLower);
    if ((it != defaultPorts.end()) && (it->second != port.value())) {
      // Add port only if it's not the default port for the scheme
      url += ":" + std::to_string(port.value());
    }
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
                                                           {"mqtts", 8883},
                                                           {"coap", 5683},       {"amqp", 5671},  // AMQP over TLS
                                                           {"rsync", 873},       {"rdp", 3389},        {"elasticsearch", 9200},
                                                           {"kibana", 5601},     {"zookeeper", 2181}};

}  // namespace athenasip::types
