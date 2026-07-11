//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>

#include "../loggers/logger.h"
#include "datastore.h"
#include "mysql_datastore.h"
#include "postgres_datastore.h"
#include "redis_datastore.h"
#include "sqlite_datastore.h"

namespace athenasip::datastores {

void register_builtin_datastores(std::shared_ptr<loggers::Logger> logger) {
  Datastore::register_driver<MySQLDatastore>(logger, "mysql");
  Datastore::register_driver<MySQLDatastore>(logger, "mysqlx");

  Datastore::register_driver<PostgreSQLDatastore>(logger, "postgres");
  Datastore::register_driver<PostgreSQLDatastore>(logger, "postgresql");

  Datastore::register_driver<SQLiteDatastore>(logger, "sqlite");
  Datastore::register_driver<SQLiteDatastore>(logger, "sqlite3");

  Datastore::register_driver<RedisDatastore>(logger, "redis");
  Datastore::register_driver<RedisDatastore>(logger, "rediss");
  Datastore::register_driver<RedisDatastore>(logger, "redis+ssl");
}

}  // namespace athenasip::datastores
