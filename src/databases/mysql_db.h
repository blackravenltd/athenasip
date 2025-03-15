//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <mysqlx/xdevapi.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <string>

#include "../util.h"
#include "db.h"

using namespace athenasip::types;
using namespace athenasip::loggers;

namespace athenasip::databases {

class MySQLDB : public DB {
 public:
  MySQLDB(std::shared_ptr<Logger> logger, std::shared_ptr<URL> url);

  virtual bool connect() override;
  virtual std::shared_ptr<DBResult> query(std::string sql, std::vector<std::any> params) override;
  virtual void close() override;

 private:
  std::shared_ptr<URL> _url;

  std::shared_ptr<DBValue> _map_value(const mysqlx::Value& val);
  std::shared_ptr<mysqlx::Session> _session;

  std::string _to_mysql_datetime_string(const std::chrono::system_clock::time_point& v);
  std::string _to_mysql_datetime_string(const std::time_t& v);
};

}  // namespace athenasip::databases
