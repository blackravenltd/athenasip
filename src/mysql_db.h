//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <mysqlx/xdevapi.h>

#include <string>

#include "db.h"

using namespace athenasip::siptypes;

namespace athenasip {

class MySQLDB : public DB {
 public:
  MySQLDB(std::shared_ptr<Logger> logger, std::shared_ptr<URL> url);
  ~MySQLDB();

  virtual bool connect() override;
  virtual std::shared_ptr<DBResult> query(std::string sql, std::vector<std::any> params) override;
  virtual void close() override;

 private:
  std::shared_ptr<URL> _url;

  std::shared_ptr<DBValue> _map_value(const mysqlx::Value &val);
  std::shared_ptr<mysqlx::Session> _session;
};

}  // namespace athenasip