//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

class SDP {
 public:
  // Session-level parameters
  std::string version;                  // v=
  std::string origin;                   // o=
  std::string sessionName;              // s=
  std::string sessionInfo;              // i=
  std::string uri;                      // u=
  std::string email;                    // e=
  std::string phone;                    // p=
  std::string connection;               // c=
  std::vector<std::string> bandwidth;   // b=
  std::string timing;                   // t=
  std::vector<std::string> repeats;     // r=
  std::string timezones;                // z=
  std::string encryption;               // k=
  std::vector<std::string> attributes;  // a= at session level

  // Media-level parameters
  struct Media {
    std::string media;                    // m=
    std::string title;                    // i=
    std::string connection;               // c=
    std::vector<std::string> bandwidth;   // b=
    std::string encryption;               // k=
    std::vector<std::string> attributes;  // a=
  };
  std::vector<Media> mediaDescriptions;

  // Parse a CRLF-delimited SDP document.
  // Returns true if parsing succeeds (very basic error handling here).
  bool parse(const std::string& sdpText) {
    // Clear any previous data.
    clear();

    // Split the input on CRLF.
    std::vector<std::string> lines;
    {
      size_t start = 0;
      while (true) {
        size_t pos = sdpText.find("\r\n", start);
        if (pos == std::string::npos) {
          if (start < sdpText.size()) lines.push_back(sdpText.substr(start));
          break;
        }
        lines.push_back(sdpText.substr(start, pos - start));
        start = pos + 2;
      }
    }

    bool inMediaSection = false;
    Media currentMedia;

    // Process each line.
    for (const auto& line : lines) {
      if (line.empty()) continue;
      // Each line must be at least 3 characters: "<type>=<value>"
      if (line.size() < 3 || line[1] != '=') {
        // Malformed line: skip or return false.
        continue;
      }
      char field = line[0];
      std::string value = line.substr(2);

      // If we haven't yet reached a media section, treat these as session-level.
      if (!inMediaSection) {
        switch (field) {
          case 'v':
            version = value;
            break;
          case 'o':
            origin = value;
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
            connection = value;
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
          case 'm':
            // Start media section.
            inMediaSection = true;
            currentMedia = Media();  // initialize a new media block
            currentMedia.media = value;
            break;
          default:
            // Unrecognized session-level field; ignore for now.
            break;
        }
      } else {  // in media section
        switch (field) {
          case 'm':
            // Push the previous media block, then start a new one.
            mediaDescriptions.push_back(currentMedia);
            currentMedia = Media();
            currentMedia.media = value;
            break;
          case 'i':
            currentMedia.title = value;
            break;
          case 'c':
            currentMedia.connection = value;
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
            // Ignore fields that are unexpected in media block.
            break;
        }
      }
    }

    // If we ended in a media section, save the last media block.
    if (inMediaSection) {
      mediaDescriptions.push_back(currentMedia);
    }

    return true;
  }

  // Return the SDP document as a CRLF-delimited string.
  std::string to_string() const {
    std::string s;
    auto appendLine = [&s](const std::string& type, const std::string& value) {
      if (!value.empty()) {
        s.append(type + "=" + value + "\r\n");
      }
    };

    appendLine("v", version);
    appendLine("o", origin);
    appendLine("s", sessionName);
    appendLine("i", sessionInfo);
    appendLine("u", uri);
    appendLine("e", email);
    appendLine("p", phone);
    appendLine("c", connection);
    for (const auto& b : bandwidth) appendLine("b", b);
    appendLine("t", timing);
    for (const auto& r : repeats) appendLine("r", r);
    appendLine("z", timezones);
    appendLine("k", encryption);
    for (const auto& a : attributes) appendLine("a", a);

    // Append each media description.
    for (const auto& media : mediaDescriptions) {
      appendLine("m", media.media);
      appendLine("i", media.title);
      appendLine("c", media.connection);
      for (const auto& b : media.bandwidth) appendLine("b", b);
      appendLine("k", media.encryption);
      for (const auto& a : media.attributes) appendLine("a", a);
    }
    return s;
  }

  void print() const { std::cout << to_string() << std::endl; }

 private:
  // Helper method to clear all members.
  void clear() {
    version.clear();
    origin.clear();
    sessionName.clear();
    sessionInfo.clear();
    uri.clear();
    email.clear();
    phone.clear();
    connection.clear();
    bandwidth.clear();
    timing.clear();
    repeats.clear();
    timezones.clear();
    encryption.clear();
    attributes.clear();
    mediaDescriptions.clear();
  }
};