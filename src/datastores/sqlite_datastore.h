//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../types/url.h"
#include "../util.h"
#include "datastore.h"

namespace athenasip::datastores {

class SQLiteDatastore : public Datastore {
 public:
  SQLiteDatastore(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url);
  ~SQLiteDatastore() override;

  bool connect() override;
  void close() override;
  bool is_connected() const override;

  std::shared_ptr<types::Realm> realm_get_by_name(const std::string& realm_name) override;
  std::shared_ptr<types::Subscriber> subscriber_get(std::shared_ptr<types::SIPIdentity> identity) override;
  bool subscriber_register(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) override;
  bool subscriber_unregister(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) override;
  bool nonce_create(const std::string& nonce, const std::time_t& expires_at) override;
  bool nonce_check(std::string nonce) override;

 private:
  std::string _db_path() const;
  std::string _to_utc_datetime_string(const std::time_t& value) const;
  std::string _duration_ms(std::chrono::high_resolution_clock::time_point start) const;
  void _log_sql(const std::string& sql, const std::string& params, std::size_t rows, std::chrono::high_resolution_clock::time_point start) const;
  void _log_sql_error(const std::string& sql, const std::string& params, const std::string& error) const;
  bool _exec_simple(const std::string& sql);

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<types::URL> _url;
  sqlite3* _db{nullptr};
};

}  // namespace athenasip::datastores
