//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "mysql_datastore.h"

#include <boost/asio/consign.hpp>
#include <boost/asio/detached.hpp>
#include <boost/system/system_error.hpp>

#include <future>
#include <iomanip>
#include <sstream>
#include <tuple>
#include <utility>

using namespace athenasip::types;

namespace athenasip::datastores {

MySQLDatastore::MySQLDatastore(std::shared_ptr<Logger> logger, std::shared_ptr<MySQLDB> mysql)
    : _logger(std::make_shared<LoggerScoped>("mysql_datastore", logger)),
      _mysql(mysql) {}

std::shared_ptr<Realm> MySQLDatastore::realm_get_by_name(const std::string& realm_name) {
  std::shared_ptr<DBResult> res =
      _mysql->query("SELECT `id`,`name`, `nonce_secret`,`nonce_expiry`,`registration_timeout` FROM `realm` WHERE `realm`.`name` = ?", {realm_name});

  if (!res) {
    _logger->error("realm_get_by_name: No result from datastore: " + realm_name);
    return nullptr;
  }

  if (res->rows.size() == 0) {
    return nullptr;
  }

  if (res->rows.size() > 1) {
    _logger->warn("Duplicate realm in datastore: " + realm_name + " (" + std::to_string(res->rows.size()) + " copies)");
  }

  auto realm = std::make_shared<Realm>(res->rows[0]->column_values[1]->as<std::string>());
  realm->id = res->rows[0]->column_values[0]->as<uint64_t>();
  realm->nonce_secret = res->rows[0]->column_values[2]->as<std::string>();
  realm->nonce_expiry = res->rows[0]->column_values[3]->as<uint32_t>();
  realm->registration_timeout = res->rows[0]->column_values[4]->as<uint32_t>();
  return realm;
}

std::shared_ptr<Subscriber> MySQLDatastore::subscriber_get(std::shared_ptr<SIPIdentity> identity) {
  std::shared_ptr<DBResult> res = _mysql->query(
      "SELECT `subscriber`.`id`,`subscriber`.`name`,`subscriber`.`ha1` FROM `subscriber`,`realm` WHERE `subscriber`.`user` = ? AND `realm`.`name` = ? AND "
      "`subscriber`.`realm_id` = `realm`.`id`",
      {identity->uri->user, identity->uri->realm});

  if (res && res->rows.size() == 0) return nullptr;

  if (res->rows.size() > 1) {
    _logger->warn("Duplicate subscriber in datastore: " + identity->to_string() + " (" + std::to_string(res->rows.size()) + " copies)");
  }

  auto subscriber = std::make_shared<Subscriber>();
  auto row = res->rows[0];
  subscriber->id = row->values["id"]->as<uint64_t>();
  subscriber->identity = identity;
  subscriber->ha1 = row->values["ha1"]->as<std::string>();
  return subscriber;
}

bool MySQLDatastore::subscriber_register(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact) {
  std::shared_ptr<DBResult> res = _mysql->query("SELECT COUNT(*) FROM `location` WHERE `subscriber_id` = ? AND `user` = ? AND `host` = ? AND `port` = ?",
                                             {subscriber->id, contact->user, contact->realm, contact->port.value_or(0)});
  if (res && res->rows.size() == 0) {
    _logger->error("subscriber_register: COUNT(*) returned no rows");
    return false;
  };

  std::string is_nat = Util::is_ipv4(contact->realm) && Util::is_ipv4_private(contact->realm) ? "Y" : "N";

  if (res->rows[0]->column_values[0]->as<int64_t>() == 0) {
    // Insert a row
    _mysql->query("INSERT INTO `location` (`subscriber_id`, `user`, `host`, `port`, `registered_at`, `nat`) VALUES (?,?,?,?,NOW(),?)",
               {subscriber->id, contact->user, contact->realm, contact->port.value_or(0), is_nat});
  } else {
    // Update the row
    _mysql->query("UPDATE `location` SET `registered_at` = NOW() WHERE `subscriber_id` = ?", {subscriber->id});
  }

  return true;
}

bool MySQLDatastore::subscriber_unregister(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact) {
  _mysql->query("DELETE FROM `location` WHERE `subscriber_id` = ? AND `user` = ? AND `host` = ? AND `port` = ?",
             {subscriber->id, contact->user, contact->realm, contact->port.value_or(0)});

  return true;
}

bool MySQLDatastore::nonce_create(const std::string& nonce, const std::time_t& expires_at)  {
  _mysql->query("INSERT INTO `nonce` (`id`, `expires_at`) VALUES (?,?)", {nonce, expires_at});

  return true;
}

bool MySQLDatastore::nonce_check(std::string nonce) {
  std::shared_ptr<DBResult> res = _mysql->query("SELECT COUNT(*) FROM `nonce` WHERE `id` = ? AND `expires_at` > NOW()", {nonce});
  if (res && res->rows.size() == 0) {
    _logger->error("nonce_check: COUNT(*) returned no rows");
    return false;
  };

  return res->rows[0]->column_values[0]->as<int64_t>() == 1;
}

}