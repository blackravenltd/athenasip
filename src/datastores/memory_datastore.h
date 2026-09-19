//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../call.h"
#include "../loggers/logger.h"
#include "../loggers/logger_scoped.h"
#include "../types/url.h"
#include "datastore.h"

namespace athenasip::datastores {

// The zero-config datastore: everything lives in this process and nothing survives a
// restart. It is what a single node runs with no Redis, and what the tests run against.
class MemoryDatastore : public Datastore {
 public:
  MemoryDatastore(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url);
  ~MemoryDatastore() override;

  std::string name() const override;
  std::string version() const override;

  bool connect() override;
  void close() override;
  bool is_connected() const override;

  std::shared_ptr<types::Realm> realm_get_by_name(const std::string& realm_name) override;
  bool realm_create(std::shared_ptr<types::Realm> realm) override;
  bool realm_update(std::shared_ptr<types::Realm> realm) override;
  bool realm_delete(const std::string& realm_name) override;
  std::vector<std::shared_ptr<types::Realm>> realm_list() override;

  std::shared_ptr<types::Subscriber> subscriber_get(std::shared_ptr<types::SIPIdentity> identity) override;
  bool subscriber_create(std::shared_ptr<types::Subscriber> subscriber) override;
  bool subscriber_update(std::shared_ptr<types::Subscriber> subscriber) override;
  bool subscriber_delete(std::shared_ptr<types::SIPIdentity> identity) override;
  std::vector<std::shared_ptr<types::Subscriber>> subscriber_list(const std::string& realm_name) override;

  bool subscriber_register(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact, std::uint32_t expires_seconds,
                           const std::string& path) override;
  bool subscriber_unregister(std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact) override;
  std::vector<types::Location> location_list(std::uint64_t subscriber_id) override;

  bool nonce_create(const std::string& nonce, const std::time_t& expires_at) override;
  bool nonce_check(std::string nonce) override;

  bool call_create(std::shared_ptr<Call> call) override;
  bool call_update(std::shared_ptr<Call> call) override;
  std::shared_ptr<Call> call_get(const std::string& id) override;
  std::vector<std::shared_ptr<Call>> call_list() override;

 private:
  static std::string _subscriber_key(const std::string& realm_name, const std::string& user);
  static std::string _location_key(std::uint64_t subscriber_id, const std::string& user, const std::string& host, std::uint16_t port);

  // Called with _mutex held.
  void _prune_expired();

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<types::URL> _url;

  mutable std::mutex _mutex;
  bool _connected = false;

  std::unordered_map<std::string, std::shared_ptr<types::Realm>> _realms;
  std::unordered_map<std::string, std::shared_ptr<types::Subscriber>> _subscribers;
  std::unordered_map<std::string, types::Location> _locations;
  std::unordered_map<std::string, std::time_t> _nonces;
  std::unordered_map<std::string, std::shared_ptr<Call>> _calls;
};

}  // namespace athenasip::datastores
