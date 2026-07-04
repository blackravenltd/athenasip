//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "loggers/logger.h"

class MockLogger : public athenasip::loggers::Logger {
public:
    void debug(const std::string &log) override {}
    void info(const std::string &log) override {}
    void warn(const std::string &log) override {}
    void error(const std::string &log) override {}
    void raw(const std::string &log) override {}
    void set_level(athenasip::loggers::LogLevel level) override {}
    std::shared_ptr<athenasip::loggers::Logger> base_logger() override { return nullptr; }
};