//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "registrar.h"

namespace athenasip {

Registrar::Registrar(std::shared_ptr<Logger> logger, std::shared_ptr<DB> db) : _logger(std::make_unique<LoggerScoped>("registrar", logger)), _db(db) {}

bool Registrar::user_exists(std::shared_ptr<SIPIdentity> identity) {
  // Execute the query
  std::shared_ptr<DBResult> res = _db->query("SELECT `h1` FROM `subscriber` WHERE `user` = ? AND `realm` = ?", {identity->uri->user, identity->uri->realm});

  if (res && res->rows.size() != 0) {
    _logger->info("user_exists " + identity->to_string() + " h1 " + res->rows[0]->values["h1"]->as<std::string>());
  }
  return false;
}

std::string Registrar::user_get_h1(std::shared_ptr<SIPIdentity> identity) { return ""; }
void Registrar::user_register(std::shared_ptr<SIPIdentity> identity, std::shared_ptr<SIPUri> location) {}
const std::shared_ptr<SIPUri> Registrar::user_get_location(std::shared_ptr<SIPIdentity> identity) { return std::make_shared<SIPUri>(""); }

}  // namespace athenasip