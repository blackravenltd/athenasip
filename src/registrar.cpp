//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "registrar.h"

namespace athenasip {

Registrar::Registrar(std::shared_ptr<Logger> logger, std::shared_ptr<DB> db) : _logger(std::make_unique<LoggerScoped>("registrar", logger)), _db(db) {}

Registrar::~Registrar() {}

bool Registrar::user_exists(const SIPIdentity& identity) {
  // Execute the query
  std::shared_ptr<DBResult> res = _db->query("SELECT `h1` FROM `subscriber` WHERE `user` = ? AND `realm` = ?", {identity.uri.user().value_or(""), identity.uri.realm()});

  if (res && !res->empty()) {
    _logger->info("user_exists " + identity + " h1 " + (*res)[0]->at("h1")->get_as<std::string>());
  }
  return false;
}

std::string Registrar::user_get_h1(const SIPIdentity& identity) { return ""; }
void Registrar::user_register(const SIPIdentity& identity, const SIPUri& location) {}
const std::shared_ptr<SIPUri> Registrar::user_get_location(const SIPIdentity& identity) { return std::make_shared<SIPUri>(""); }

}  // namespace athenasip