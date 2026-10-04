//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "loggers/logger.h"

// A silent logger that records every line, for tests about what the node logs.
class MockLogger : public athenasip::loggers::Logger {
 public:
  void debug(const std::string& log) override { _record(athenasip::loggers::LogLevel::DEBUG, log); }
  void info(const std::string& log) override { _record(athenasip::loggers::LogLevel::INFO, log); }
  void warn(const std::string& log) override { _record(athenasip::loggers::LogLevel::WARN, log); }
  void error(const std::string& log) override { _record(athenasip::loggers::LogLevel::ERROR, log); }
  void raw(const std::string& log) override { _record(athenasip::loggers::LogLevel::INFO, log); }

  void set_level(athenasip::loggers::LogLevel level) override {}
  std::shared_ptr<athenasip::loggers::Logger> base_logger() override { return nullptr; }

  // Everything logged, in order, optionally of one level only.
  std::vector<std::string> lines() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _lines;
  }

  std::vector<std::string> lines(athenasip::loggers::LogLevel level) const {
    std::lock_guard<std::mutex> lock(_mutex);

    std::vector<std::string> found;
    for (std::size_t i = 0; i < _lines.size(); ++i) {
      if (_levels[i] == level) found.push_back(_lines[i]);
    }
    return found;
  }

  // The whole log as one string, for asking whether something was said at all.
  std::string text() const {
    std::lock_guard<std::mutex> lock(_mutex);

    std::string all;
    for (const auto& line : _lines) all += line + "\n";
    return all;
  }

  void clear() {
    std::lock_guard<std::mutex> lock(_mutex);
    _lines.clear();
    _levels.clear();
  }

 private:
  void _record(athenasip::loggers::LogLevel level, const std::string& log) {
    std::lock_guard<std::mutex> lock(_mutex);
    _lines.push_back(log);
    _levels.push_back(level);
  }

  mutable std::mutex _mutex;
  std::vector<std::string> _lines;
  std::vector<athenasip::loggers::LogLevel> _levels;
};
