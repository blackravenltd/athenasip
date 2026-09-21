//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "util.h"

namespace athenasip {

// A session description, modelled the way a proxy needs it: every line is kept, in the
// order it arrived, and only the few fields we rewrite are parsed out.
//
// That ordering matters. A proxy that rebuilds SDP from a struct silently drops
// everything it does not model, which for a browser offer means ICE candidates, DTLS
// fingerprints and BUNDLE groups. RFC 8866 section 5.13 also requires a parser to
// ignore line types it does not understand, and for a proxy "ignore" means "pass on".

// One "<type>=<value>" line.
struct Line {
  char type = 0;
  std::string value;

  std::string to_string() const { return std::string(1, type) + "=" + value + "\r\n"; }
};

// A connection field, e.g. "IN IP4 192.0.2.1" (RFC 8866 section 5.7).
struct ConnectionInfo {
  std::string nettype;
  std::string addrtype;
  std::string address;

  static ConnectionInfo parse(const std::string& s) {
    ConnectionInfo ci;
    std::istringstream iss(s);
    iss >> ci.nettype >> ci.addrtype >> ci.address;
    return ci;
  }

  bool empty() const { return nettype.empty() && addrtype.empty() && address.empty(); }

  std::string to_string() const { return nettype + " " + addrtype + " " + address; }
};

// The origin field, e.g. "alice 2890844526 2890844526 IN IP4 192.0.2.1"
// (RFC 8866 section 5.2).
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

  std::string to_string() const { return username + " " + sessionId + " " + sessionVersion + " " + nettype + " " + addrtype + " " + address; }
};

// The "m=" line: "<media> <port>[/<count>] <proto> <fmt> ..." (RFC 8866 section 5.14).
// The optional port count is why the port cannot be read with a plain integer parse:
// "49170/2" stops the parse at the slash and the proto then swallows "/2".
struct MediaDescription {
  std::string media;
  std::uint16_t port = 0;
  std::uint16_t port_count = 0;  // 0 when the m= line carried no "/count"
  std::string proto;
  std::vector<std::string> formats;

  static MediaDescription parse(const std::string& s) {
    MediaDescription md;
    std::istringstream iss(s);

    std::string port_field;
    iss >> md.media >> port_field >> md.proto;

    const auto slash = port_field.find('/');
    if (slash == std::string::npos) {
      md.port = static_cast<std::uint16_t>(std::strtoul(port_field.c_str(), nullptr, 10));
    } else {
      md.port = static_cast<std::uint16_t>(std::strtoul(port_field.substr(0, slash).c_str(), nullptr, 10));
      md.port_count = static_cast<std::uint16_t>(std::strtoul(port_field.substr(slash + 1).c_str(), nullptr, 10));
    }

    std::string fmt;
    while (iss >> fmt) md.formats.push_back(fmt);

    return md;
  }

  std::string to_string() const {
    std::ostringstream oss;
    oss << media << " " << port;
    if (port_count > 0) oss << "/" << port_count;
    oss << " " << proto;
    for (const auto& f : formats) oss << " " << f;
    return oss.str();
  }
};

// One media section: its "m=" line plus every line that follows, in order, until the
// next "m=" or the end.
class MediaSection {
 public:
  MediaDescription description;

  const std::vector<Line>& lines() const { return _lines; }
  std::vector<Line>& lines() { return _lines; }

  void add_line(const Line& line) { _lines.push_back(line); }

  bool has_connection() const { return _find('c') != nullptr; }

  ConnectionInfo connection() const {
    const auto* line = _find('c');
    return line ? ConnectionInfo::parse(line->value) : ConnectionInfo();
  }

  // Replaces the media-level c=, or inserts one in the position RFC 8866 section 5
  // gives it: after i=, before b=, k= and a=.
  void set_connection(const ConnectionInfo& connection) {
    for (auto& line : _lines) {
      if (line.type == 'c') {
        line.value = connection.to_string();
        return;
      }
    }

    auto at = _lines.begin();
    while (at != _lines.end() && at->type == 'i') ++at;
    _lines.insert(at, Line{'c', connection.to_string()});
  }

  std::vector<std::string> attributes() const {
    std::vector<std::string> result;
    for (const auto& line : _lines) {
      if (line.type == 'a') result.push_back(line.value);
    }
    return result;
  }

  // Replaces the first a= whose value starts with the prefix. Returns false when there
  // was none, so the caller can decide whether to add one.
  bool set_attribute(const std::string& prefix, const std::string& value) {
    for (auto& line : _lines) {
      if (line.type == 'a' && line.value.rfind(prefix, 0) == 0) {
        line.value = value;
        return true;
      }
    }
    return false;
  }

  void add_attribute(const std::string& value) { _lines.push_back(Line{'a', value}); }

  // The BUNDLE identifier for this section (RFC 8843), empty when absent.
  std::string mid() const { return _attribute_after("mid:"); }

  // The first rtpmap codec, for identifying the stream across re-offers.
  std::string codec() const {
    const auto rtpmap = _attribute_after("rtpmap:");
    if (rtpmap.empty()) return "";

    const auto space = rtpmap.find(' ');
    return space == std::string::npos ? "" : rtpmap.substr(space + 1);
  }

  // Stable across a re-offer that keeps the same stream, so media can be matched up.
  std::int64_t unique_id() const {
    std::size_t seed = 0;
    seed = Util::hash_combine(seed, std::hash<std::string>{}(mid()));
    seed = Util::hash_combine(seed, std::hash<std::string>{}(description.media));
    seed = Util::hash_combine(seed, std::hash<std::string>{}(codec()));
    return static_cast<std::int64_t>(seed);
  }

  std::string to_string() const {
    std::ostringstream oss;
    oss << "m=" << description.to_string() << "\r\n";
    for (const auto& line : _lines) oss << line.to_string();
    return oss.str();
  }

 private:
  std::vector<Line> _lines;

  const Line* _find(char type) const {
    for (const auto& line : _lines) {
      if (line.type == type) return &line;
    }
    return nullptr;
  }

  std::string _attribute_after(const std::string& prefix) const {
    for (const auto& line : _lines) {
      if (line.type == 'a' && line.value.rfind(prefix, 0) == 0) return line.value.substr(prefix.size());
    }
    return "";
  }
};

class SDP {
 public:
  // Returns false when the text is not a session description. RFC 8866 section 5 makes
  // v=, o=, s= and t= mandatory, so a description missing any of them is rejected
  // rather than quietly half-parsed.
  bool parse(const std::string& text) {
    _clear();

    std::istringstream stream(text);
    std::string line;
    bool in_media = false;

    while (std::getline(stream, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty()) continue;

      // Every line is "<type>=<value>" with a single-character type.
      if (line.size() < 2 || line[1] != '=') return false;

      const char type = line[0];
      const std::string value = line.substr(2);

      if (type == 'm') {
        in_media = true;
        _media.emplace_back();
        _media.back().description = MediaDescription::parse(value);
        continue;
      }

      if (in_media) {
        _media.back().add_line(Line{type, value});
      } else {
        _session_lines.push_back(Line{type, value});
      }
    }

    _valid = _has_session_line('v') && _has_session_line('o') && _has_session_line('s') && _has_session_line('t');
    return _valid;
  }

  bool is_valid() const { return _valid; }

  bool has_connection() const { return _find_session('c') != nullptr; }

  ConnectionInfo connection() const {
    const auto* line = _find_session('c');
    return line ? ConnectionInfo::parse(line->value) : ConnectionInfo();
  }

  // Replaces the session-level c=, or inserts one where RFC 8866 section 5 puts it:
  // after s=, i=, u=, e= and p=, before b=, t= and the rest.
  void set_connection(const ConnectionInfo& connection) {
    for (auto& line : _session_lines) {
      if (line.type == 'c') {
        line.value = connection.to_string();
        return;
      }
    }

    static const std::string before_c = "vosiuep";

    auto at = _session_lines.begin();
    while (at != _session_lines.end() && before_c.find(at->type) != std::string::npos) ++at;
    _session_lines.insert(at, Line{'c', connection.to_string()});
  }

  Origin origin() const {
    const auto* line = _find_session('o');
    return line ? Origin::parse(line->value) : Origin();
  }

  // Replaces the o= line. It never inserts one: RFC 8866 section 5 makes the field
  // mandatory and parse() refuses a description without it, so there is always one to
  // replace and a description that reached here has passed that.
  void set_origin(const Origin& origin) {
    for (auto& line : _session_lines) {
      if (line.type == 'o') {
        line.value = origin.to_string();
        return;
      }
    }
  }

  std::vector<std::string> session_attributes() const {
    std::vector<std::string> result;
    for (const auto& line : _session_lines) {
      if (line.type == 'a') result.push_back(line.value);
    }
    return result;
  }

  const std::vector<Line>& session_lines() const { return _session_lines; }

  std::vector<MediaSection>& media() { return _media; }
  const std::vector<MediaSection>& media() const { return _media; }

  // Re-emits every line in the order it arrived, so anything not modelled comes out
  // exactly as it went in.
  std::string to_string() const {
    std::ostringstream oss;

    for (const auto& line : _session_lines) {
      // RFC 8866 section 5.3: s= must carry at least one character. "-" is the
      // conventional filler, and emitting a bare "s=" would be invalid.
      if (line.type == 's' && line.value.empty()) {
        oss << "s=-\r\n";
        continue;
      }

      oss << line.to_string();
    }

    for (const auto& section : _media) oss << section.to_string();
    return oss.str();
  }

  void print() const { std::cout << to_string() << std::endl; }

 private:
  std::vector<Line> _session_lines;
  std::vector<MediaSection> _media;
  bool _valid = false;

  void _clear() {
    _session_lines.clear();
    _media.clear();
    _valid = false;
  }

  const Line* _find_session(char type) const {
    for (const auto& line : _session_lines) {
      if (line.type == type) return &line;
    }
    return nullptr;
  }

  bool _has_session_line(char type) const { return _find_session(type) != nullptr; }
};

}  // namespace athenasip
