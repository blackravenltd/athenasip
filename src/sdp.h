//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <functional>

#include "./util.h"

namespace athenasip {


//
// Represents a connection field, e.g. "IN IP4 192.168.18.100"
//
struct ConnectionInfo {
  std::string nettype;   // e.g. "IN"
  std::string addrtype;  // e.g. "IP4"
  std::string address;   // e.g. "192.168.18.100"

  static ConnectionInfo parse(const std::string& s) {
    ConnectionInfo ci;
    std::istringstream iss(s);
    iss >> ci.nettype >> ci.addrtype >> ci.address;
    return ci;
  }

  std::string to_string() const { 
    return nettype + " " + addrtype + " " + address; 
  }
};

//
// Represents the origin field, e.g. "tom 1744 1438 IN IP4 192.168.18.52"
//
struct Origin {
  std::string username;
  std::string sessionId;
  std::string sessionVersion;
  std::string nettype;
  std::string addrtype;
  std::string address;

  static Origin parse(const std::string& s) {
    Origin o;
    std::istringstream iss(s);
    iss >> o.username >> o.sessionId >> o.sessionVersion >> o.nettype >> o.addrtype >> o.address;
    return o;
  }

  std::string to_string() const { 
    return username + " " + sessionId + " " + sessionVersion + " " + nettype + " " + addrtype + " " + address; 
  }
};

//
// Represents the "m=" media description (e.g. "audio 50000 RTP/AVP 96 97 98 0 8 ...")
//
struct MediaDescription {
  std::string media;                 // media type (audio, video, etc.)
  uint16_t port;                     // port number
  std::string proto;                 // protocol (e.g. RTP/AVP)
  std::vector<std::string> formats;  // payload types

  static MediaDescription parse(const std::string& s) {
    MediaDescription md;
    std::istringstream iss(s);
    iss >> md.media >> md.port >> md.proto;
    std::string fmt;
    while (iss >> fmt) {
      md.formats.push_back(fmt);
    }
    return md;
  }

  std::string to_string() const {
    std::ostringstream oss;
    oss << media << " " << port << " " << proto;
    for (const auto& f : formats) oss << " " << f;
    return oss.str();
  }
};

//
// Represents a complete media section, including optional fields like media-level connection.
//
struct Media {
  MediaDescription description;
  std::string title;          // i=
  ConnectionInfo connection;  // c= (optional; overrides session-level)
  bool hasConnection = false;
  std::vector<std::string> bandwidth;   // b=
  std::string encryption;               // k=
  std::vector<std::string> attributes;  // a=

  std::string to_string() const {
    std::ostringstream oss;
    oss << "m=" << description.to_string() << "\r\n";
    if (!title.empty()) oss << "i=" << title << "\r\n";
    if (hasConnection) oss << "c=" << connection.to_string() << "\r\n";
    for (const auto& b : bandwidth) oss << "b=" << b << "\r\n";
    if (!encryption.empty()) oss << "k=" << encryption << "\r\n";
    for (const auto& a : attributes) oss << "a=" << a << "\r\n";
    return oss.str();
  }

  // Extract the "mid" attribute from this media, if present.
  std::string extract_mid() {
    const std::string prefix = "mid:";
    for (const auto &attr : attributes) {
      if (attr.compare(0, prefix.size(), prefix) == 0) {
        return attr.substr(prefix.size());
      }
    }
    return "";
  }

  // Extract the codec from the first "rtpmap:" attribute.
  std::string extract_codec() {
    const std::string prefix = "rtpmap:";
    for (const auto &attr : attributes) {
      if (attr.compare(0, prefix.size(), prefix) == 0) {
        // Expected format: "rtpmap:<pt> <codec>/<clockrate>"
        auto pos = attr.find(' ');
        if (pos != std::string::npos && pos + 1 < attr.size()) {
          return attr.substr(pos + 1);
        }
      }
    }
    return "";
  }

  int64_t get_unique_id() {
      int64_t seed = 0;
      seed = Util::hash_combine(seed, std::hash<std::string>{}(extract_mid()));
      seed = Util::hash_combine(seed, std::hash<std::string>{}(description.media));
      seed = Util::hash_combine(seed, std::hash<std::string>{}(extract_codec()));
      return seed;
  }
};

//
// Represents an entire SDP, including session-level fields and media sections.
//
class SDP {
 public:
  // Session-level fields.
  std::string version;        // v=
  Origin origin;              // o=
  std::string sessionName;    // s=
  std::string sessionInfo;    // i=
  std::string uri;            // u=
  std::string email;          // e=
  std::string phone;          // p=
  ConnectionInfo connection;  // c=
  bool hasConnection = false;
  std::vector<std::string> bandwidth;   // b=
  std::string timing;                   // t=
  std::vector<std::string> repeats;     // r=
  std::string timezones;                // z=
  std::string encryption;               // k=
  std::vector<std::string> attributes;  // a=

  // Media sections.
  std::vector<Media> mediaDescriptions;

  // Parses the given SDP text (assumed to be CRLF-delimited).
  bool parse(const std::string& sdpText) {
    clear();
    std::istringstream sdpStream(sdpText);
    std::string line;
    bool inMediaSection = false;
    Media currentMedia;

    while (std::getline(sdpStream, line)) {
      // Remove trailing carriage return if present.
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.size() < 2 || line[1] != '=') continue;  // skip malformed lines

      char type = line[0];
      std::string value = line.substr(2);

      if (!inMediaSection) {
        switch (type) {
          case 'v':
            version = value;
            break;
          case 'o':
            origin = Origin::parse(value);
            break;
          case 's':
            sessionName = value;
            break;
          case 'i':
            sessionInfo = value;
            break;
          case 'u':
            uri = value;
            break;
          case 'e':
            email = value;
            break;
          case 'p':
            phone = value;
            break;
          case 'c':
            connection = ConnectionInfo::parse(value);
            hasConnection = true;
            break;
          case 'b':
            bandwidth.push_back(value);
            break;
          case 't':
            timing = value;
            break;
          case 'r':
            repeats.push_back(value);
            break;
          case 'z':
            timezones = value;
            break;
          case 'k':
            encryption = value;
            break;
          case 'a':
            attributes.push_back(value);
            break;
          case 'm': {
            inMediaSection = true;
            currentMedia = Media();
            currentMedia.description = MediaDescription::parse(value);
            break;
          }
          default:
            break;
        }
      } else {
        switch (type) {
          case 'm':
            // New media block begins; save the current one.
            mediaDescriptions.push_back(currentMedia);
            currentMedia = Media();
            currentMedia.description = MediaDescription::parse(value);
            break;
          case 'i':
            currentMedia.title = value;
            break;
          case 'c':
            currentMedia.connection = ConnectionInfo::parse(value);
            currentMedia.hasConnection = true;
            break;
          case 'b':
            currentMedia.bandwidth.push_back(value);
            break;
          case 'k':
            currentMedia.encryption = value;
            break;
          case 'a':
            currentMedia.attributes.push_back(value);
            break;
          default:
            break;
        }
      }
    }
    if (inMediaSection) {
      mediaDescriptions.push_back(currentMedia);
    }
    return true;
  }

  // Serializes the SDP back into a CRLF-delimited string.
  std::string to_string() const {
    std::ostringstream oss;
    oss << "v=" << version << "\r\n";
    oss << "o=" << origin.to_string() << "\r\n";
    oss << "s=" << sessionName << "\r\n";
    if (!sessionInfo.empty()) oss << "i=" << sessionInfo << "\r\n";
    if (!uri.empty()) oss << "u=" << uri << "\r\n";
    if (!email.empty()) oss << "e=" << email << "\r\n";
    if (!phone.empty()) oss << "p=" << phone << "\r\n";
    if (hasConnection) oss << "c=" << connection.to_string() << "\r\n";
    for (const auto& b : bandwidth) oss << "b=" << b << "\r\n";
    oss << "t=" << timing << "\r\n";
    for (const auto& r : repeats) oss << "r=" << r << "\r\n";
    if (!timezones.empty()) oss << "z=" << timezones << "\r\n";
    if (!encryption.empty()) oss << "k=" << encryption << "\r\n";
    for (const auto& a : attributes) oss << "a=" << a << "\r\n";

    for (const auto& media : mediaDescriptions) oss << media.to_string();
    return oss.str();
  }

  void print() const { std::cout << to_string() << std::endl; }

 private:
  void clear() {
    version.clear();
    origin = Origin();
    sessionName.clear();
    sessionInfo.clear();
    uri.clear();
    email.clear();
    phone.clear();
    connection = ConnectionInfo();
    hasConnection = false;
    bandwidth.clear();
    timing.clear();
    repeats.clear();
    timezones.clear();
    encryption.clear();
    attributes.clear();
    mediaDescriptions.clear();
  }
};

}