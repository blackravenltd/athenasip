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

  virtual bool connect() override;
  virtual std::shared_ptr<DBResult> query(std::string sql, std::vector<std::any> params) override;
  virtual void close() override;

 private:
  std::shared_ptr<URL> _url;
  std::shared_ptr<pqxx::connection> _connection;

  // Map a PostgreSQL field to a DBValue based on the PostgreSQL column type.
  std::shared_ptr<DBValue> _map_value(const pqxx::field &field, const std::string &colType);
};

}  // namespace athenasip::databases
