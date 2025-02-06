//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <string>

namespace athenasip {

enum LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

class Logger {
 public:
  virtual ~Logger() {}
  virtual void debug(const std::string &log) = 0;
  virtual void info(const std::string &log) = 0;
  virtual void warn(const std::string &log) = 0;
  virtual void error(const std::string &log) = 0;
  virtual void raw(const std::string &log) = 0;
};

}  // namespace athenasip