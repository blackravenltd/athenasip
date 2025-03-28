//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
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

  std::unordered_map<std::string, std::string> mime_types = {
      {".html", "text/html"},        {".htm", "text/html"}, {".css", "text/css"},   {".js", "application/javascript"},
      {".json", "application/json"}, {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
      {".gif", "image/gif"},         {".svg", "image/svg"}};
};

namespace athenasip::api {

class StaticMiddleware {
 public:
  // This function is intended to be a static member of your server class (e.g. AdminAPI).
  // It returns a HttpMiddleware function that serves static files from disk.
  static HttpMiddleware add(const std::string &base_path, const StaticOptions &opts = StaticOptions()) {
    return
        [base_path, opts](const http::request<http::string_body> &req, std::shared_ptr<http::response<http::string_body>> res, std::function<void(bool)> next) {
          // Convert the request target to a string.
          std::string target = std::string(req.target());

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

          // 3. Construct the file path.
          // If relative_path is empty or ends with '/', try default files.
          if (relative_path.empty() || (!relative_path.empty() && relative_path.back() == '/')) {
            bool found = false;
            for (const auto &df : opts.default_file) {
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
          }

          // 4. Ensure the file is within the public base directory.
          try {
            fs::path canonical_base = fs::canonical(base_path);
            fs::path canonical_file = fs::canonical(full_path);
            if (canonical_file.string().find(canonical_base.string()) != 0) {
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