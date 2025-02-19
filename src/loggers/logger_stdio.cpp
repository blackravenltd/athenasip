//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "logger_stdio.h"

namespace athenasip::loggers {

LoggerStdIO::LoggerStdIO(LogLevel log_level) { _log_level = log_level; }

void LoggerStdIO::debug(const std::string &log) {
  if (_log_level > LogLevel::DEBUG) return;
  std::lock_guard<std::mutex> lock(mtx);
  std::cout << _getDateTime() << " [DEBUG] " << log << std::endl;
}

void LoggerStdIO::info(const std::string &log) {
  if (_log_level > LogLevel::INFO) return;
  std::lock_guard<std::mutex> lock(mtx);
  std::cout << _getDateTime() << " [INFO ] " << log << std::endl;
}

void LoggerStdIO::warn(const std::string &log) {
  if (_log_level > LogLevel::WARN) return;
  std::lock_guard<std::mutex> lock(mtx);
  std::cout << _getDateTime() << " [WARN ] " << log << std::endl;
}

void LoggerStdIO::error(const std::string &log) {
  std::lock_guard<std::mutex> lock(mtx);
  std::cout << _getDateTime() << " [ERROR] " << log << std::endl;
}

void LoggerStdIO::raw(const std::string &log) {
  std::lock_guard<std::mutex> lock(mtx);
  std::cout << _getDateTime() << " [-----] " << log << std::endl;
}

const std::string LoggerStdIO::_getDateTime() {
  auto now = std::chrono::system_clock::now();
  auto now_c = std::chrono::system_clock::to_time_t(now);
  std::tm now_tm = *std::gmtime(&now_c);

  std::ostringstream oss;
  oss << std::put_time(&now_tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

std::shared_ptr<Logger> LoggerStdIO::base_logger() { return nullptr; }

void LoggerStdIO::set_level(LogLevel level) {
  _log_level = level;
};

}  // namespace athenasip::loggers