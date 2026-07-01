//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <string>

#include "logger.h"

namespace athenasip::loggers {

class LoggerScoped : public Logger {
 public:
  LoggerScoped(std::string scope, std::shared_ptr<Logger> logger);

  virtual void debug(const std::string& log) override;
  virtual void info(const std::string& log) override;
  virtual void warn(const std::string& log) override;
  virtual void error(const std::string& log) override;
  virtual void raw(const std::string& log) override;
  virtual std::shared_ptr<Logger> base_logger() override;
  virtual void set_level(LogLevel level) override;

 private:
  std::string _scope;
  std::shared_ptr<Logger> _logger;
};

}  // namespace athenasip::loggers