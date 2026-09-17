//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../call.h"
#include "../loggers/logger.h"
#include "../types/location.h"
#include "../types/realm.h"
#include "../types/sip_identity.h"
#include "../types/sip_uri.h"
#include "../types/subscriber.h"
#include "../types/url.h"

namespace athenasip::datastores {

class Datastore {
 public:
  virtual ~Datastore() = default;

  virtual std::string get_driver_name() const = 0;

  virtual bool connect() = 0;
  virtual void close() = 0;
  virtual bool is_connected() const = 0;

  // Realms. create and update are separate so provisioning can tell "already exists"
  // from "changed", which the admin API needs to answer 409 rather than overwrite.
  virtual std::shared_ptr<types::Realm> realm_get_by_name(const std::string& realm_name) = 0;
  virtual bool realm_create(std::shared_ptr<types::Realm> realm) = 0;
  virtual bool realm_update(std::shared_ptr<types::Realm> realm) = 0;
  virtual bool realm_delete(const std::string& realm_name) = 0;
  virtual std::vector<std::shared_ptr<types::Realm>> realm_list() = 0;

  // Subscribers.
  virtual std::shared_ptr<types::Subscriber> subscriber_get(std::shared_ptr<types::SIPIdentity> identity) = 0;
  virtual bool subscriber_create(std::shared_ptr<types::Subscriber> subscriber) = 0;
  virtual bool subscriber_update(std::shared_ptr<types::Subscriber> subscriber) = 0;
  virtual bool subscriber_delete(std::shared_ptr<types::SIPIdentity> identity) = 0;
  virtual std::vector<std::shared_ptr<types::Subscriber>> subscriber_list(const std::string& realm_name) = 0;

  // Registrations (RFC 3261 section 10 bindings).
  virtual bool subscriber_register(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) = 0;
  virtual bool subscriber_unregister(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) = 0;

  // Every live binding for a subscriber. Target determination needs all of them
  // (RFC 3261 16.5). Expired bindings are not returned.
  virtual std::vector<types::Location> location_list(std::uint64_t subscriber_id) = 0;

  virtual bool nonce_create(const std::string& nonce, const std::time_t& expires_at) = 0;
  virtual bool nonce_check(std::string nonce) = 0;

  // Calls.
  virtual bool call_create(std::shared_ptr<Call>) = 0;
  virtual bool call_update(std::shared_ptr<Call>) = 0;
  virtual std::shared_ptr<Call> call_get(const std::string& id) = 0;
  virtual std::vector<std::shared_ptr<Call>> call_list() = 0;

  template <typename T, typename = std::enable_if_t<std::is_base_of_v<Datastore, T>>>
  static void register_driver(std::shared_ptr<loggers::Logger> logger, std::string scheme) {
    auto& drivers = get_drivers();
    logger->debug("(datastore) Registering scheme " + scheme);

    drivers[std::move(scheme)] = [](std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url) -> std::shared_ptr<Datastore> {
      return std::static_pointer_cast<Datastore>(std::make_shared<T>(std::move(logger), std::move(url)));
    };
  }

  static std::shared_ptr<Datastore> create_driver(std::shared_ptr<loggers::Logger> logger, const std::string& url_string) {
    auto url = std::make_shared<types::URL>(url_string);

    logger->debug("(datastore) Finding scheme " + url->scheme);

    auto& drivers = get_drivers();
    auto it = drivers.find(url->scheme);
    if (it == drivers.end()) {
      logger->error("(datastore) Unknown scheme " + url->scheme);
      return nullptr;
    }

    return it->second(std::move(logger), std::move(url));
  }

 protected:
  using Factory = std::function<std::shared_ptr<Datastore>(std::shared_ptr<loggers::Logger>, std::shared_ptr<types::URL>)>;

  static std::unordered_map<std::string, Factory>& get_drivers() {
    static std::unordered_map<std::string, Factory> drivers;
    return drivers;
  }
};

}  // namespace athenasip::datastores
