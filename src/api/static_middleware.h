//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/beast/http.hpp>
#include <boost/json.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "./admin_api.h"

namespace http = boost::beast::http;

struct StaticOptions {
  std::vector<std::string> default_file{"index.html", "index.htm"};
  std::string prefix{"/"};

  // For a client-side router: a GET for an extensionless path that is not on disk is
  // answered with this document. Paths with an extension are never substituted, so a
  // missing asset stays a 404. Empty turns it off.
  std::string fallback{"index.html"};

  // Prefixes the fallback never applies under: an unknown API path stays a 404.
  std::vector<std::string> fallback_excludes{"api/"};

  // Browsers refuse module scripts and SVG served with the wrong type, so every file type
  // a built web client uses must be listed.
  std::unordered_map<std::string, std::string> mime_types = {{".html", "text/html"},
                                                             {".htm", "text/html"},
                                                             {".css", "text/css"},
                                                             {".js", "application/javascript"},
                                                             {".mjs", "application/javascript"},
                                                             {".json", "application/json"},
                                                             {".map", "application/json"},
                                                             {".png", "image/png"},
                                                             {".jpg", "image/jpeg"},
                                                             {".jpeg", "image/jpeg"},
                                                             {".gif", "image/gif"},
                                                             {".svg", "image/svg+xml"},
                                                             {".ico", "image/x-icon"},
                                                             {".woff", "font/woff"},
                                                             {".woff2", "font/woff2"},
                                                             {".wasm", "application/wasm"},
                                                             {".txt", "text/plain"}};
};

namespace athenasip::api {

class StaticMiddleware {
 public:
  // Returns a middleware that serves static files from base_path.
  static HttpMiddleware add(const std::string& base_path, const StaticOptions& opts = StaticOptions()) {
    return
        [base_path, opts](const http::request<http::string_body>& req, const std::string&, std::shared_ptr<http::response<http::string_body>> res,
                         std::function<void(bool)> next) {
          // The target is a URI reference: drop the query and fragment (RFC 3986 3.4, 3.5),
          // so the cache-busting "bundle.js?v=2" serves bundle.js.
          std::string target = std::string(req.target());
          if (const auto separator = target.find_first_of("?#"); separator != std::string::npos) target.erase(separator);

          // Percent-encoding is deliberately not decoded: nothing under a document root needs
          // it, and decoding would have to come before the traversal check below.

          if (target.find(opts.prefix) != 0) {
            next(true);
            return;
          }

          std::string relative_path = target.substr(opts.prefix.size());

          // Reject traversal and home-directory segments.
          std::istringstream iss(relative_path);
          std::string segment;
          while (std::getline(iss, segment, '/')) {
            if (segment == ".." || segment == "." || (!segment.empty() && segment[0] == '~')) {
              next(true);
              return;
            }
          }

          namespace fs = std::filesystem;
          std::string full_path;

          // Whether a path with no file behind it may be answered with the fallback document.
          const auto routable = [&]() {
            if (opts.fallback.empty()) return false;

            // Page views only.
            if (req.method() != http::verb::get && req.method() != http::verb::head) return false;

            for (const auto& excluded : opts.fallback_excludes) {
              if (relative_path.rfind(excluded, 0) == 0) return false;
            }

            // An empty segment ("//etc/hosts") is a malformed path or a probe, not a client
            // route.
            std::istringstream segments(relative_path);
            std::string segment;
            std::string last_segment;

            while (std::getline(segments, segment, '/')) {
              if (segment.empty()) return false;
              last_segment = segment;
            }

            // An extension in the last segment means an asset was asked for by name.
            return !last_segment.empty() && last_segment.find('.') == std::string::npos;
          }();

          // Substitutes the fallback document for the path; it then passes the same
          // document-root check as any other file.
          const auto route_or_pass_on = [&]() {
            if (!routable) return false;

            const std::string candidate = base_path + "/" + opts.fallback;
            if (!fs::exists(candidate) || !fs::is_regular_file(candidate)) return false;

            relative_path = opts.fallback;
            full_path = candidate;
            return true;
          };

          // A directory path serves the first default file that exists.
          if (relative_path.empty() || (!relative_path.empty() && relative_path.back() == '/')) {
            bool found = false;
            for (const auto& df : opts.default_file) {
              std::string candidate = base_path;
              if (!relative_path.empty())
                candidate += "/" + relative_path + df;
              else
                candidate += "/" + df;
              if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
                full_path = candidate;
                found = true;
                break;
              }
            }
            if (!found) {
              next(true);
              return;
            }
          } else {
            full_path = base_path + "/" + relative_path;

            if (!fs::exists(full_path) && !route_or_pass_on()) {
              next(true);
              return;
            }
          }

          // The file must be inside the document root. Canonicalising first catches symlinks,
          // and comparing as paths rather than strings keeps "/srv/public-secrets" out of
          // "/srv/public".
          try {
            const fs::path canonical_base = fs::canonical(base_path);
            const fs::path canonical_file = fs::canonical(full_path);

            const auto within = canonical_file.lexically_relative(canonical_base);

            if (within.empty() || within.begin()->string() == "..") {
              next(true);
              return;
            }
          } catch (...) {
            next(true);
            return;
          }

          if (!fs::exists(full_path) || !fs::is_regular_file(full_path)) {
            next(true);
            return;
          }

          std::ifstream file(full_path, std::ios::binary | std::ios::ate);
          if (!file) {
            next(true);
            return;
          }
          auto file_size = file.tellg();
          file.seekg(0, std::ios::beg);
          std::string file_content(file_size, '\0');
          if (!file.read(&file_content[0], file_size)) {
            next(true);
            return;
          }

          res->body() = file_content;
          res->set(http::field::content_length, std::to_string(file_size));

          std::string mime_type = "application/octet-stream";
          auto dot_pos = full_path.find_last_of('.');
          if (dot_pos != std::string::npos) {
            std::string ext = full_path.substr(dot_pos);
            auto it = opts.mime_types.find(ext);
            if (it != opts.mime_types.end()) mime_type = it->second;
          }
          res->set(http::field::content_type, mime_type);

          next(false);
        };
  }
};

}  // namespace athenasip::api