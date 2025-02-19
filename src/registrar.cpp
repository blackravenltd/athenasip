//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "registrar.h"

using namespace athenasip::databases;

namespace athenasip {

Registrar::Registrar(std::shared_ptr<Logger> logger, std::shared_ptr<athenasip::databases::DB> db)
    : _logger(std::make_unique<LoggerScoped>("registrar", logger)), _db(db) {}

bool Registrar::subscriber_exists(std::shared_ptr<SIPIdentity> identity) {
  std::shared_ptr<DBResult> res = _db->query("SELECT COUNT(*) FROM `subscriber` WHERE `user` = ? AND `realm` = ?", {identity->uri->user, identity->uri->realm});

  if (res && res->rows.size() == 0) {
    _logger->warn("DB returned no rows on a COUNT() statement");
    return false;
  } else {
    return res->rows[0]->column_values[0]->as<long long>() == 1;
  }
}

std::shared_ptr<Subscriber> Registrar::subscriber_get(std::shared_ptr<SIPIdentity> identity) {
  std::shared_ptr<DBResult> res =
      _db->query("SELECT `id`,`name`,`h1` FROM `subscriber` WHERE `user` = ? AND `realm` = ?", {identity->uri->user, identity->uri->realm});

  if (res && res->rows.size() == 0) return nullptr;

  auto obj = std::make_shared<Subscriber>();
  auto row = res->rows[0];
  obj->id = row->values["id"]->as<uint64_t>();
  obj->identity = identity;
  obj->h1 = row->values["h1"]->as<std::string>();
  return obj;
}

bool Registrar::subscriber_register(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact) {
  std::shared_ptr<DBResult> res = _db->query("SELECT COUNT(*) FROM `location` WHERE `subscriber_id` = ? AND `user` = ? AND `host` = ? AND `port` = ?",
                                             {subscriber->id, contact->user, contact->realm, contact->port.value_or(0)});
  if (res && res->rows.size() == 0) {
    _logger->error("subscriber_register: COUNT(*) returned no rows");
    return false;
  };

  std::string is_nat = Util::is_ipv4(contact->realm) && Util::is_ipv4_private(contact->realm) ? "Y" : "N";

  if (res->rows[0]->column_values[0]->as<long long>() == 0) {
    // Insert a row
    _db->query("INSERT INTO `location` (`subscriber_id`, `user`, `host`, `port`, `registered_at`, `nat`) VALUES (?,?,?,?,NOW(),?)",
               {subscriber->id, contact->user, contact->realm, contact->port.value_or(0), is_nat});
  } else {
    // Update the row
    _db->query("UPDATE `location` SET `registered_at` = NOW() WHERE `id` = ?) VALUES (?,?,?,?,?,?)", {subscriber->id});
  }

  return true;
}

bool Registrar::register_session(std::string endpoint, std::shared_ptr<Session> session) {
  std::lock_guard<std::shared_mutex> lock(_sessions_mutex);
  _sessions.insert({endpoint, session});
  _logger->debug("Registered Connection " + endpoint);
  return true;
}

bool Registrar::unregister_session(std::string endpoint, std::shared_ptr<Session> session) {
  std::lock_guard<std::shared_mutex> lock(_sessions_mutex);
  // _sessions.erase(endpoint);
  _logger->debug("Unregistered Connection " + endpoint);
  return true;
}

void Registrar::session_close_all() {
    // Close All Connections
  for (const auto& pair : _sessions) pair.second->close();

  // Remove all connections
  std::unique_lock<std::shared_mutex> lock(_sessions_mutex);
  _sessions.clear();
  lock.unlock();
}

const std::shared_ptr<SIPUri> Registrar::subscriber_get_location(std::shared_ptr<SIPIdentity> identity) { return std::make_shared<SIPUri>(""); }

}  // namespace athenasip