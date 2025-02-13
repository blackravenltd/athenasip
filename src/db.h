//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <mysqlx/xdevapi.h>

#include <any>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "db_types.h"
#include "logger.h"
#include "logger_scoped.h"
#include "url.h"

namespace athenasip {

class DB {
 public:
  DB(std::shared_ptr<Logger> logger) : _logger(logger) {};

  virtual bool connect() = 0;
  virtual std::shared_ptr<DBResult> query(std::string sql, std::vector<std::any> params) = 0;
  virtual void close() = 0;

  // Register a Database Driver
  template <typename T, typename = std::enable_if_t<std::is_base_of<DB, T>::value>>
  static void register_driver(std::string scheme) {
    auto &drivers = get_drivers();
    drivers[scheme] = [](std::shared_ptr<Logger> logger, std::shared_ptr<URL> url) -> std::shared_ptr<DB> {
      return std::static_pointer_cast<DB>(std::make_shared<T>(logger, url));
    };
  }

  // Get a driver instance by name
  static std::shared_ptr<DB> create_driver(std::shared_ptr<Logger> logger, std::string url) {
    auto _url = std::make_shared<URL>(url);
    auto &drivers = get_drivers();
    auto it = drivers.find(_url->scheme);
    if (it != drivers.end()) {
      return it->second(logger, _url);  // Call the stored factory function
    } else {
      std::cerr << "[DB] create_driver: Unknown scheme: " << _url->scheme << std::endl;
      return nullptr;
    }
  }

 protected:
  std::shared_ptr<Logger> _logger;

  static std::unordered_map<std::string, std::function<std::shared_ptr<DB>(std::shared_ptr<Logger> logger, std::shared_ptr<URL> url)>> &get_drivers() {
    static std::unordered_map<std::string, std::function<std::shared_ptr<DB>(std::shared_ptr<Logger> logger, std::shared_ptr<URL> url)>> drivers;
    return drivers;
  }
};

}  // namespace athenasip