//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../databases/mysql_db.h"

#include "datastore.h"

using namespace athenasip::loggers;
using namespace athenasip::databases;

namespace athenasip::datastores {

class MySQLDatastore : public Datastore {
 public:
 	MySQLDatastore(std::shared_ptr<Logger> logger, std::shared_ptr<MySQLDB> mysql);

  virtual std::shared_ptr<Realm> realm_get_by_name(const std::string& realm_name) override;
	virtual std::shared_ptr<Subscriber> subscriber_get(std::shared_ptr<SIPIdentity> identity) override;
	virtual bool subscriber_register(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact) override;
	virtual bool subscriber_unregister(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact) override;
	virtual bool nonce_create(const std::string& nonce, const std::time_t& expires_at) override;
	virtual bool nonce_check(std::string nonce) override;

 private:
 	std::shared_ptr<Logger> _logger;
 	std::shared_ptr<MySQLDB> _mysql;
};

}