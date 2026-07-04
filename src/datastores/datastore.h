//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>
#include <string>

#include "datastore.h"

#include "../types/realm.h"
#include "../types/sip_uri.h"
#include "../types/sip_identity.h"
#include "../types/subscriber.h"

using namespace athenasip::types;

namespace athenasip::datastores {

// Abstract base class for a datastore
class Datastore {
 public:
  virtual ~Datastore() = default;

  virtual std::shared_ptr<Realm> realm_get_by_name(const std::string& realm_name) = 0;
	virtual std::shared_ptr<Subscriber> subscriber_get(std::shared_ptr<SIPIdentity> identity) = 0;
	virtual bool subscriber_register(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact) = 0;
	virtual bool subscriber_unregister(std::shared_ptr<Subscriber> subscriber, std::shared_ptr<SIPUri> contact) = 0;
	virtual bool nonce_create(const std::string& nonce, const std::time_t& expires_at) = 0;
	virtual bool nonce_check(std::string nonce) = 0;
};

}