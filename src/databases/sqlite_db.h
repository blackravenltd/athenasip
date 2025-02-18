///
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <sqlite3.h>
#include <memory>
#include <string>
#include <vector>
#include <any>
#include <unordered_map>
#include <chrono>
#include <sstream>
#include <filesystem>

#include "db.h"
#include "../util.h"

using namespace athenasip::types;
using namespace athenasip::loggers;

namespace athenasip::databases {

class SQLiteDB : public DB {
 public:
  SQLiteDB(std::shared_ptr<Logger> logger, std::shared_ptr<URL> url);
  ~SQLiteDB();

  virtual bool connect() override;
  virtual std::shared_ptr<DBResult> query(std::string sql, std::vector<std::any> params) override;
  virtual void close() override;

 private:
  std::shared_ptr<URL> _url;
  sqlite3* _db;

  std::shared_ptr<DBValue> _map_value(sqlite3_value* val);
  void _bind_parameters(sqlite3_stmt* stmt, const std::vector<std::any>& params);
};

}  // namespace athenasip
