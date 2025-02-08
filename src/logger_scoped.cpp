//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "logger_scoped.h"

#include "logger.h"

namespace athenasip {

LoggerScoped::LoggerScoped(std::string scope, std::shared_ptr<Logger> logger) : _scope(scope), _logger(logger) {}

void LoggerScoped::debug(const std::string &str) { _logger->debug("(" + _scope + ") " + str); }
void LoggerScoped::info(const std::string &str) { _logger->info("(" + _scope + ") " + str); }
void LoggerScoped::warn(const std::string &str) { _logger->warn("(" + _scope + ") " + str); }
void LoggerScoped::error(const std::string &str) { _logger->error("(" + _scope + ") " + str); }
void LoggerScoped::raw(const std::string &str) { _logger->error("(" + _scope + ") " + str); }

std::shared_ptr<Logger> LoggerScoped::base_logger() { return _logger; }

}  // namespace athenasip