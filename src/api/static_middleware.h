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

// Assuming these namespaces are already defined:
namespace http = boost::beast::http;

// Options struct for the static middleware.
struct StaticOptions {
  std::vector<std::string> default_file{"index.html", "index.htm"};
  std::string prefix{"/"};

  // A client-side router keeps real paths, and a reload of one asks this node for a
  // document that is not on disk. Answering it with the document that knows how to
  // route it is what makes every page of the client work when it is arrived at
  // directly rather than only when it is navigated to.
  //
  // Only for a path with no extension, because a missing asset has to stay missing: a
  // bundle that answers 200 with HTML in it is far worse to debug than one that 404s.
  // Empty turns it off.
  std::string fallback{"index.html"};

  // Prefixes the fallback never applies under. An unknown path below the API is a 404
  // about the API, and answering it with a web page would be a lie about what this node
  // serves.
  std::vector<std::string> fallback_excludes{"api/"};

  // What a built web client is made of. A browser refuses a module script that does not
  // arrive as JavaScript and will not render an SVG that does not arrive as
  // image/svg+xml, so a type missing here is a file the client cannot use.
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
  // This function is intended to be a static member of your server class (e.g. AdminAPI).
  // It returns a HttpMiddleware function that serves static files from disk.
  static HttpMiddleware add(const std::string& base_path, const StaticOptions& opts = StaticOptions()) {
    return
        [base_path, opts](const http::request<http::string_body>& req, std::shared_ptr<http::response<http::string_body>> res, std::function<void(bool)> next) {
          // The target is a URI reference, not a path. A query and a fragment are
          // separate components of it (RFC 3986 sections 3.4 and 3.5) and neither is
          // part of the file being asked for, so "bundle.js?v=2" is a request for
          // bundle.js. Every built web application cache-busts this way, and the admin
          // client is one.
          std::string target = std::string(req.target());
          if (const auto separator = target.find_first_of("?#"); separator != std::string::npos) target.erase(separator);

          // Percent-encoding is deliberately not decoded. Nothing under a document root
          // needs it, and a decoder here would have to be followed by the traversal
          // check below rather than preceded by it, which is the order that has caught
          // out every server that got this wrong.

          // 1. If the request target does not begin with opts.prefix, skip processing.
          if (target.find(opts.prefix) != 0) {
            next(true);
            return;
          }

          // 2. Remove the prefix from the target to get the relative path.
          std::string relative_path = target.substr(opts.prefix.size());

          // Sanitize: split the relative path into segments and reject any with forbidden parts.
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

          // Whether a path with nothing behind it should be answered with the routing
          // document. Decided here, where the relative path is known and before any of
          // the ways of not finding a file.
          const auto routable = [&]() {
            if (opts.fallback.empty()) return false;

            // A page view and nothing else. A write to a path that does not exist is
            // not one, whatever the path looks like.
            if (req.method() != http::verb::get && req.method() != http::verb::head) return false;

            for (const auto& excluded : opts.fallback_excludes) {
              if (relative_path.rfind(excluded, 0) == 0) return false;
            }

            // A route is made of names. An empty segment means the path was malformed
            // rather than navigated to - "//etc/hosts" is somebody trying the root as a
            // prefix, not a page of the client - and standing in for it would answer a
            // probe with 200.
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

          // The routing document stands in for the path, so from here on it is what is
          // being served: its own type, its own contents, and the same document-root
          // check as anything else.
          const auto route_or_pass_on = [&]() {
            if (!routable) return false;

            const std::string candidate = base_path + "/" + opts.fallback;
            if (!fs::exists(candidate) || !fs::is_regular_file(candidate)) return false;

            relative_path = opts.fallback;
            full_path = candidate;
            return true;
          };

          // 3. Construct the file path.
          // If relative_path is empty or ends with '/', try default files.
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

          // 4. Ensure the file really is within the document root. Resolving it first
          // is what catches a symlink, whose path says nothing about where it goes.
          //
          // Compared as paths and not as strings: "/srv/public-secrets/x" begins with
          // "/srv/public" and is not inside it, so a prefix comparison hands out a
          // sibling directory to anybody who can get a link into the root.
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

          // 5. Check if the file exists and is a regular file.
          if (!fs::exists(full_path) || !fs::is_regular_file(full_path)) {
            next(true);
            return;
          }

          // 6. Open and read the file content.
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

          // 7. Set the response body and headers.
          res->body() = file_content;
          res->set(http::field::content_length, std::to_string(file_size));

          // Determine MIME type based on file extension.
          std::string mime_type = "application/octet-stream";
          auto dot_pos = full_path.find_last_of('.');
          if (dot_pos != std::string::npos) {
            std::string ext = full_path.substr(dot_pos);
            auto it = opts.mime_types.find(ext);
            if (it != opts.mime_types.end()) mime_type = it->second;
          }
          res->set(http::field::content_type, mime_type);

          // Serve it
          next(false);
        };
  }
};

}  // namespace athenasip::api