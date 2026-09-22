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
#include "memory_datastore.h"
#include "redis_datastore.h"

namespace athenasip::datastores {

// Two implementations: one built-in for a zero-config single node, one canonical for
// production. The SQL drivers are gone; a memory:// driver replaces them.
inline void register_builtin_datastores(std::shared_ptr<loggers::Logger> logger) {
  Datastore::register_driver<MemoryDatastore>(logger, "memory");

  Datastore::register_driver<RedisDatastore>(logger, "redis");
  Datastore::register_driver<RedisDatastore>(logger, "rediss");
  Datastore::register_driver<RedisDatastore>(logger, "redis+ssl");
}

}  // namespace athenasip::datastores
