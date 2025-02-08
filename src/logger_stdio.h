//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <mutex>
#include <string>

#include "logger.h"

namespace athenasip {

class LoggerStdIO : public Logger {
 public:
  LoggerStdIO(LogLevel log_level);

  virtual void debug(const std::string &log);
  virtual void info(const std::string &log);
  virtual void warn(const std::string &log);
  virtual void error(const std::string &log);
  virtual void raw(const std::string &log);
  virtual std::shared_ptr<Logger> base_logger();

 private:
  LogLevel _log_level;
  const std::string _getDateTime();
  std::mutex mtx;
};

}  // namespace athenasip
