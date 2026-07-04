//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "redis_datastore.h"

#include <boost/asio/consign.hpp>
#include <boost/asio/detached.hpp>
#include <boost/redis/src.hpp>
#include <boost/system/system_error.hpp>

#include <future>
#include <iomanip>
#include <sstream>
#include <tuple>
#include <utility>

using namespace athenasip::types;

namespace athenasip::datastores {

RedisDatastore::RedisDatastore(std::shared_ptr<Logger> logger, std::shared_ptr<RedisDB> redis)
    : _logger(std::make_shared<LoggerScoped>("redis_datastore", logger)),
      _redis(redis) {}

std::shared_ptr<Realm> RedisDatastore::realm_get_by_name(const std::string& realm_name) {

}

std::shared_ptr<Subscriber> RedisDatastore::subscriber_get(std::shared_ptr<SIPIdentity> identity) {

}

bool RedisDatastore::subscriber_register(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact) {

}

bool RedisDatastore::subscriber_unregister(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact) {

}

bool RedisDatastore::nonce_create(const std::string& nonce, const std::time_t& expires_at)  {

}

bool RedisDatastore::nonce_check(std::string nonce) {

}

}