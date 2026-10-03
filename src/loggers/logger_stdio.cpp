//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "logger_stdio.h"

#include <boost/json.hpp>

#include "../util.h"

namespace athenasip::loggers {

LoggerStdIO::LoggerStdIO(LogLevel log_level, std::ostream& out) : _log_level(log_level), _out(out) {}

void LoggerStdIO::debug(const std::string& log) {
  if (_log_level > LogLevel::DEBUG) return;
  _write("[DEBUG]", "debug", log);
}

void LoggerStdIO::info(const std::string& log) {
  if (_log_level > LogLevel::INFO) return;
  _write("[INFO ]", "info", log);
}

void LoggerStdIO::warn(const std::string& log) {
  if (_log_level > LogLevel::WARN) return;
  _write("[WARN ]", "warn", log);
}

void LoggerStdIO::error(const std::string& log) { _write("[ERROR]", "error", log); }

void LoggerStdIO::raw(const std::string& log) { _write("[-----]", "raw", log); }

// A scoped logger writes its scope as "(scope) " in front of the line, one for each scope
// it is nested in, outermost first. In JSON those come off the front and become a field of
// their own, so a shipper can filter by component without parsing the message.
void LoggerStdIO::_write(const char* text_level, const char* json_level, const std::string& log) {
  std::lock_guard<std::mutex> lock(mtx);

  if (_format == LogFormat::Text) {
    _out << Util::get_zulu_time() << " " << text_level << " " << log << std::endl;
    return;
  }

  std::string scope;
  std::size_t at = 0;
  while (at < log.size() && log[at] == '(') {
    const auto close = log.find(") ", at);
    if (close == std::string::npos) break;

    if (!scope.empty()) scope += "/";
    scope += log.substr(at + 1, close - at - 1);
    at = close + 2;
  }

  boost::json::object line;
  line["at"] = Util::get_zulu_time();
  line["level"] = json_level;
  if (!scope.empty()) line["scope"] = scope;
  line["message"] = log.substr(at);

  _out << boost::json::serialize(line) << std::endl;
}

std::shared_ptr<Logger> LoggerStdIO::base_logger() { return nullptr; }

void LoggerStdIO::set_level(LogLevel level) { _log_level = level; };

void LoggerStdIO::set_format(LogFormat format) { _format = format; }

}  // namespace athenasip::loggers
