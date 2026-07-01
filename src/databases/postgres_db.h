//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <any>
#include <memory>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "../util.h"
#include "db.h"

using namespace athenasip::types;
using namespace athenasip::loggers;

namespace athenasip::databases {

class PostgreSQLDB : public DB {
 public:
  PostgreSQLDB(std::shared_ptr<Logger> logger, std::shared_ptr<URL> url);
  ~PostgreSQLDB();

  bool connect() override;
  std::shared_ptr<DBResult> query(std::string sql, std::vector<std::any> params) override;
  void close() override;
  bool is_created() override;

 private:
  std::shared_ptr<URL> _url;
  std::shared_ptr<pqxx::connection> _connection;

  std::shared_ptr<DBValue> _map_value(pqxx::field_ref field, const std::string& colType);
};

}  // namespace athenasip::databases
