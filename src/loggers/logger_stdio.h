//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

#include "logger.h"

namespace athenasip::loggers {

class LoggerStdIO : public Logger {
 public:
  // Writes to standard output by default; tests pass their own stream.
  explicit LoggerStdIO(LogLevel log_level, std::ostream& out = std::cout);

  virtual void debug(const std::string& log) override;
  virtual void info(const std::string& log) override;
  virtual void warn(const std::string& log) override;
  virtual void error(const std::string& log) override;
  virtual void raw(const std::string& log) override;
  virtual std::shared_ptr<Logger> base_logger() override;
  virtual void set_level(LogLevel level) override;

  // Set once the configuration has been read.
  void set_format(LogFormat format);

 private:
  void _write(const char* text_level, const char* json_level, const std::string& log);

  LogLevel _log_level;
  LogFormat _format = LogFormat::Text;
  std::ostream& _out;
  std::mutex mtx;
};

}  // namespace athenasip::loggers
