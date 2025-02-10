//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <mysqlx/xdevapi.h>

#include <unordered_map>
#include <vector>

#include "db.h"
#include "db_types.h"
#include "logger.h"
#include "logger_scoped.h"
#include "url.h"

namespace athenasip {

class MySQLDB : public DB {
 public:
  MySQLDB(std::shared_ptr<Logger> logger, URL &db_url);
  ~MySQLDB();

  virtual bool connect();
  virtual std::shared_ptr<DBResult> query(std::string sql, std::vector<std::any> params);
  virtual void close();

 private:
  std::shared_ptr<DBValue> _map_value(const mysqlx::Value &val);
  std::shared_ptr<mysqlx::Session> _session;
};

}  // namespace athenasip