//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "provisioning_api.h"

#include <memory>
#include <utility>
#include <vector>

#include "../loggers/logger_scoped.h"
#include "../types/sip_identity.h"
#include "../util.h"
#include "api_json.h"

namespace athenasip::api {

namespace {

// An account is identified by the realm it lives in and the user within it, which is
// exactly a SIP URI. The API takes the two apart in the path because a path segment
// with an @ in it reads badly; this puts them back together.
std::shared_ptr<types::SIPIdentity> identity_of(const std::string& realm_name, const std::string& user) {
  return std::make_shared<types::SIPIdentity>("sip:" + user + "@" + realm_name);
}

std::string uri_of(const std::string& realm_name, const std::string& user) { return "sip:" + user + "@" + realm_name; }

// RFC 2617 / RFC 7616: HA1 is the hash of user, realm and password, and it is the only
// form of the password this server ever holds. A password given to the API is turned
// into one here and is not written down anywhere else.
std::string ha1_of(const std::string& user, const std::string& realm_name, const std::string& password) {
  return Util::to_lower(Util::md5(user + ":" + realm_name + ":" + password));
}

// RFC 8760, the same thing with SHA-256. Both are computed while the password is in
// hand, because neither can be derived from the other afterwards: an account
// provisioned with a password can answer either challenge, and one imported as a bare
// MD5 hash can only ever answer that one.
std::string ha1_sha256_of(const std::string& user, const std::string& realm_name, const std::string& password) {
  return Util::to_lower(Util::sha256(user + ":" + realm_name + ":" + password));
}

// The realm's media policy, as two independent fields: whether to anchor, and how a
// leg's profile is decided. Only what was given, like everything else here.
void read_media_policy(const boost::json::object& body, types::MediaPolicy& policy) {
  if (const auto anchor = body.if_contains("media_anchor"); anchor != nullptr && anchor->is_bool()) policy.anchor = anchor->as_bool();

  if (const auto profiles = string_field(body, "media_profiles")) {
    policy.profiles = types::MediaPolicy::profiles_from_string(*profiles, policy.profiles);
  }
}

}  // namespace

ProvisioningAPI::ProvisioningAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor,
                                 std::shared_ptr<Config> config, std::string version)
    : _logger(std::make_shared<loggers::LoggerScoped>("provisioning", std::move(logger))),
      _datastore(std::move(datastore)),
      _executor(std::move(executor)),
      _config(std::move(config)),
      _version(std::move(version)) {}

void ProvisioningAPI::register_routes(Router& router) {
  auto self = shared_from_this();

  // Open, because a container healthcheck and a load balancer reach it before they have
  // any credentials to present. It says the node is up and what it is; nothing about
  // who is on it.
  router.add(http::verb::get, "/api/v1/health", Router::public_scope, [self](RouteContext c) { self->_health(std::move(c)); });

  // client, not admin: this is what a client reads to know where the realm is served
  // from, and it says nothing about who is on it.
  router.add(http::verb::get, "/api/v1/nodes", "client", [self](RouteContext c) { self->_node_list(std::move(c)); });

  router.add(http::verb::get, "/api/v1/realms", "admin", [self](RouteContext c) { self->_realm_list(std::move(c)); });
  router.add(http::verb::post, "/api/v1/realms", "admin", [self](RouteContext c) { self->_realm_create(std::move(c)); });
  router.add(http::verb::get, "/api/v1/realms/{realm}", "admin", [self](RouteContext c) { self->_realm_get(std::move(c)); });
  router.add(http::verb::put, "/api/v1/realms/{realm}", "admin", [self](RouteContext c) { self->_realm_update(std::move(c)); });
  router.add(http::verb::delete_, "/api/v1/realms/{realm}", "admin", [self](RouteContext c) { self->_realm_delete(std::move(c)); });

  router.add(http::verb::get, "/api/v1/realms/{realm}/accounts", "admin", [self](RouteContext c) { self->_account_list(std::move(c)); });
  router.add(http::verb::post, "/api/v1/realms/{realm}/accounts", "admin", [self](RouteContext c) { self->_account_create(std::move(c)); });
  router.add(http::verb::get, "/api/v1/realms/{realm}/accounts/{user}", "admin", [self](RouteContext c) { self->_account_get(std::move(c)); });
  router.add(http::verb::put, "/api/v1/realms/{realm}/accounts/{user}", "admin", [self](RouteContext c) { self->_account_update(std::move(c)); });
  router.add(http::verb::delete_, "/api/v1/realms/{realm}/accounts/{user}", "admin", [self](RouteContext c) { self->_account_delete(std::move(c)); });

  // client rather than admin: where a subscriber is registered is what a client needs
  // to show a presence list, and it provisions nothing.
  router.add(http::verb::get, "/api/v1/registrations", "client", [self](RouteContext c) { self->_registration_list(std::move(c)); });
}

// Realms

void ProvisioningAPI::_realm_list(RouteContext context) {
  _datastore->realm_list(_executor, [context](plugins::Result<std::vector<std::shared_ptr<types::Realm>>> result) mutable {
    if (!result.ok) {
      write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
      return context.done();
    }

    boost::json::array realms;
    for (const auto& realm : result.value) {
      if (realm) realms.push_back(_realm_json(*realm));
    }

    write_json(context.response, http::status::ok, realms);
    context.done();
  });
}

void ProvisioningAPI::_realm_create(RouteContext context) {
  const auto body = parse_object(context.body);
  if (!body) {
    write_error(context.response, http::status::bad_request, "invalid_json", "the body is not a JSON object");
    return context.done();
  }

  const auto name = string_field(*body, "name");
  if (!name || name->empty()) {
    write_error(context.response, http::status::bad_request, "invalid_request", "name is required");
    return context.done();
  }

  auto realm = std::make_shared<types::Realm>(*name);
  realm->id = Util::stable_id("realm:" + *name);

  // A realm with no secret mints nonces keyed on nothing, so one is generated rather
  // than left empty. It is never returned: the only thing that needs it is this node.
  realm->nonce_secret = string_field(*body, "nonce_secret").value_or(Util::generate_random_string("", 32));

  if (const auto expiry = uint_field(*body, "nonce_expiry")) realm->nonce_expiry = *expiry;
  if (const auto timeout = uint_field(*body, "registration_timeout")) realm->registration_timeout = *timeout;
  if (const auto minimum = uint_field(*body, "registration_minimum")) realm->registration_minimum = *minimum;

  read_media_policy(*body, realm->media);

  auto self = shared_from_this();
  _datastore->realm_create(_executor, realm, [self, context, realm](plugins::Status status) mutable {
    if (!status.ok) {
      return self->_fail(std::move(context), status, "conflict", "realm " + realm->name + " already exists", http::status::conflict);
    }

    write_json(context.response, http::status::created, _realm_json(*realm));
    context.done();
  });
}

void ProvisioningAPI::_realm_get(RouteContext context) {
  const auto name = context.parameter("realm");

  _with_realm(name, std::move(context), [](std::shared_ptr<types::Realm> realm, RouteContext context) {
    write_json(context.response, http::status::ok, _realm_json(*realm));
    context.done();
  });
}

void ProvisioningAPI::_realm_update(RouteContext context) {
  const auto body = parse_object(context.body);
  if (!body) {
    write_error(context.response, http::status::bad_request, "invalid_json", "the body is not a JSON object");
    return context.done();
  }

  const auto realm_name = context.parameter("realm");

  auto self = shared_from_this();
  _with_realm(realm_name, std::move(context), [self, body](std::shared_ptr<types::Realm> realm, RouteContext context) {
    // Only what was given. A PUT that left out a field and silently reset it to the
    // default would be a way to lose a nonce secret without being told.
    if (const auto secret = string_field(*body, "nonce_secret")) realm->nonce_secret = *secret;
    if (const auto expiry = uint_field(*body, "nonce_expiry")) realm->nonce_expiry = *expiry;
    if (const auto timeout = uint_field(*body, "registration_timeout")) realm->registration_timeout = *timeout;
    if (const auto minimum = uint_field(*body, "registration_minimum")) realm->registration_minimum = *minimum;

    read_media_policy(*body, realm->media);

    self->_datastore->realm_update(self->_executor, realm, [context, realm](plugins::Status status) mutable {
      if (!status.ok) {
        write_error(context.response, http::status::internal_server_error, "datastore_error", status.error);
        return context.done();
      }

      write_json(context.response, http::status::ok, _realm_json(*realm));
      context.done();
    });
  });
}

void ProvisioningAPI::_realm_delete(RouteContext context) {
  const auto realm_name = context.parameter("realm");

  auto self = shared_from_this();

  _with_realm(realm_name, std::move(context), [self](std::shared_ptr<types::Realm> realm, RouteContext context) {
    self->_datastore->realm_delete(self->_executor, realm->name, [context](plugins::Status status) mutable {
      if (!status.ok) {
        write_error(context.response, http::status::internal_server_error, "datastore_error", status.error);
        return context.done();
      }

      write_no_content(context.response);
      context.done();
    });
  });
}

// Accounts

void ProvisioningAPI::_account_list(RouteContext context) {
  const auto realm_name = context.parameter("realm");

  auto self = shared_from_this();

  _with_realm(realm_name, std::move(context), [self](std::shared_ptr<types::Realm> realm, RouteContext context) {
    self->_datastore->account_list(self->_executor, realm->name, [context](plugins::Result<std::vector<std::shared_ptr<types::Account>>> result) mutable {
      if (!result.ok) {
        write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
        return context.done();
      }

      boost::json::array accounts;
      for (const auto& account : result.value) {
        if (account) accounts.push_back(_account_json(*account));
      }

      write_json(context.response, http::status::ok, accounts);
      context.done();
    });
  });
}

void ProvisioningAPI::_account_create(RouteContext context) {
  const auto body = parse_object(context.body);
  if (!body) {
    write_error(context.response, http::status::bad_request, "invalid_json", "the body is not a JSON object");
    return context.done();
  }

  const auto realm_name = context.parameter("realm");

  auto self = shared_from_this();
  _with_realm(realm_name, std::move(context), [self, body](std::shared_ptr<types::Realm> realm, RouteContext context) {
    const auto user = string_field(*body, "user");
    if (!user || user->empty()) {
      write_error(context.response, http::status::bad_request, "invalid_request", "user is required");
      return context.done();
    }

    const auto password = string_field(*body, "password");
    const auto ha1 = string_field(*body, "ha1");

    // ha1 is for importing accounts from somewhere that already holds one. Either is
    // enough; neither is an account that can never authenticate.
    if (!password && !ha1) {
      write_error(context.response, http::status::bad_request, "invalid_request", "password or ha1 is required");
      return context.done();
    }

    auto account = std::make_shared<types::Account>();
    account->identity = identity_of(realm->name, *user);
    account->id = Util::stable_id(uri_of(realm->name, *user));

    if (password) {
      account->ha1 = ha1_of(*user, realm->name, *password);
      account->ha1_sha256 = ha1_sha256_of(*user, realm->name, *password);
    }

    // An explicit hash wins over one derived from a password, which is what makes an
    // import of either kind work.
    if (ha1) account->ha1 = *ha1;
    if (const auto imported = string_field(*body, "ha1_sha256")) account->ha1_sha256 = *imported;

    self->_datastore->account_create(self->_executor, account, [self, context, account](plugins::Status status) mutable {
      if (!status.ok) {
        return self->_fail(std::move(context), status, "conflict", "that account already exists", http::status::conflict);
      }

      write_json(context.response, http::status::created, _account_json(*account));
      context.done();
    });
  });
}

void ProvisioningAPI::_account_get(RouteContext context) {
  const auto realm_name = context.parameter("realm");
  const auto user = context.parameter("user");

  _datastore->account_get(_executor, identity_of(realm_name, user), [context](plugins::Result<std::shared_ptr<types::Account>> result) mutable {
    if (!result.ok) {
      write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
      return context.done();
    }

    if (!result.value) {
      write_error(context.response, http::status::not_found, "not_found", "no such account");
      return context.done();
    }

    write_json(context.response, http::status::ok, _account_json(*result.value));
    context.done();
  });
}

void ProvisioningAPI::_account_update(RouteContext context) {
  const auto body = parse_object(context.body);
  if (!body) {
    write_error(context.response, http::status::bad_request, "invalid_json", "the body is not a JSON object");
    return context.done();
  }

  const auto realm_name = context.parameter("realm");
  const auto user = context.parameter("user");

  auto self = shared_from_this();
  _datastore->account_get(_executor, identity_of(realm_name, user),
                          [self, context, body, realm_name, user](plugins::Result<std::shared_ptr<types::Account>> result) mutable {
                            if (!result.ok) {
                              write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
                              return context.done();
                            }

                            if (!result.value) {
                              write_error(context.response, http::status::not_found, "not_found", "no such account");
                              return context.done();
                            }

                            auto account = result.value;

                            if (const auto password = string_field(*body, "password")) {
                              account->ha1 = ha1_of(user, realm_name, *password);
                              account->ha1_sha256 = ha1_sha256_of(user, realm_name, *password);
                            }

                            if (const auto ha1 = string_field(*body, "ha1")) account->ha1 = *ha1;
                            if (const auto sha256 = string_field(*body, "ha1_sha256")) account->ha1_sha256 = *sha256;

                            self->_datastore->account_update(self->_executor, account, [context, account](plugins::Status status) mutable {
                              if (!status.ok) {
                                write_error(context.response, http::status::internal_server_error, "datastore_error", status.error);
                                return context.done();
                              }

                              write_json(context.response, http::status::ok, _account_json(*account));
                              context.done();
                            });
                          });
}

void ProvisioningAPI::_account_delete(RouteContext context) {
  const auto realm_name = context.parameter("realm");
  const auto user = context.parameter("user");

  auto self = shared_from_this();
  auto identity = identity_of(realm_name, user);

  // Delete answers "it did not happen" for a account that was never there, and that is
  // a 404 rather than a 500. The read is what tells the two apart.
  _datastore->account_get(_executor, identity, [self, context, identity](plugins::Result<std::shared_ptr<types::Account>> result) mutable {
    if (!result.ok) {
      write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
      return context.done();
    }

    if (!result.value) {
      write_error(context.response, http::status::not_found, "not_found", "no such account");
      return context.done();
    }

    self->_datastore->account_delete(self->_executor, identity, [context](plugins::Status status) mutable {
      if (!status.ok) {
        write_error(context.response, http::status::internal_server_error, "datastore_error", status.error);
        return context.done();
      }

      write_no_content(context.response);
      context.done();
    });
  });
}

// Registrations

void ProvisioningAPI::_registration_list(RouteContext context) {
  auto self = shared_from_this();

  // One realm when asked for one, every realm otherwise. Walking them is a read per
  // account and this is a diagnostic rather than something on the call path, so the
  // cost is the caller's to choose.
  const auto realm_filter = context.query.find("realm");
  const auto wanted = realm_filter == context.query.end() ? std::string() : realm_filter->second;

  auto registrations = std::make_shared<boost::json::array>();

  auto finish = [context, registrations]() mutable {
    write_json(context.response, http::status::ok, *registrations);
    context.done();
  };

  // Each step starts the next from its own completion: with nothing to block on, a walk
  // is a chain.
  auto walk_accounts = [self, registrations](std::vector<std::shared_ptr<types::Account>> accounts, std::function<void()> done) {
    struct Walk : std::enable_shared_from_this<Walk> {
      std::shared_ptr<ProvisioningAPI> api;
      std::shared_ptr<boost::json::array> out;
      std::vector<std::shared_ptr<types::Account>> accounts;
      std::function<void()> done;
      std::size_t index = 0;

      void step() {
        if (index >= accounts.size()) return done();

        auto account = accounts[index++];
        if (!account || !account->identity || !account->identity->uri) return step();

        auto self = shared_from_this();
        const auto uri = account->identity->uri->to_string();

        api->_datastore->location_list(api->_executor, account->id, [self, uri](plugins::Result<std::vector<types::Location>> result) mutable {
          if (result.ok) {
            for (const auto& location : result.value) self->out->push_back(_location_json(location, uri));
          }

          self->step();
        });
      }
    };

    auto walk = std::make_shared<Walk>();
    walk->api = self;
    walk->out = registrations;
    walk->accounts = std::move(accounts);
    walk->done = std::move(done);
    walk->step();
  };

  if (!wanted.empty()) {
    _datastore->account_list(_executor, wanted, [context, walk_accounts, finish](plugins::Result<std::vector<std::shared_ptr<types::Account>>> result) mutable {
      if (!result.ok) {
        write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
        return context.done();
      }

      walk_accounts(std::move(result.value), finish);
    });
    return;
  }

  _datastore->realm_list(_executor, [self, context, walk_accounts, finish](plugins::Result<std::vector<std::shared_ptr<types::Realm>>> result) mutable {
    if (!result.ok) {
      write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
      return context.done();
    }

    auto realms = std::make_shared<std::vector<std::shared_ptr<types::Realm>>>(std::move(result.value));
    auto index = std::make_shared<std::size_t>(0);

    auto next = std::make_shared<std::function<void()>>();
    *next = [self, realms, index, walk_accounts, finish, next]() mutable {
      if (*index >= realms->size()) return finish();

      auto realm = (*realms)[(*index)++];
      if (!realm) return (*next)();

      self->_datastore->account_list(self->_executor, realm->name,
                                     [walk_accounts, next](plugins::Result<std::vector<std::shared_ptr<types::Account>>> accounts) mutable {
                                       if (!accounts.ok) return (*next)();

                                       walk_accounts(std::move(accounts.value), [next]() { (*next)(); });
                                     });
    };

    (*next)();
  });
}

void ProvisioningAPI::_health(RouteContext context) {
  boost::json::object health;
  health["status"] = _datastore->is_connected() ? "ok" : "degraded";
  health["node"] = _config->sip_node_id;
  health["version"] = _version;
  health["datastore"] = _datastore->describe();

  write_json(context.response, _datastore->is_connected() ? http::status::ok : http::status::service_unavailable, health);
  context.done();
}

void ProvisioningAPI::_node_list(RouteContext context) {
  boost::json::object node;
  node["id"] = _config->sip_node_id;

  // Every node answers "this is me" about itself. A client that reads the list from one
  // node and finds no entry marked self is talking to something that is not a node.
  node["self"] = true;
  node["transports"] = _local_transports();

  boost::json::array nodes;
  nodes.push_back(std::move(node));

  write_json(context.response, http::status::ok, nodes);
  context.done();
}

boost::json::array ProvisioningAPI::_local_transports() const {
  boost::json::array transports;

  // sip.public_address when it is set, because a node bound to 0.0.0.0 knows every
  // address it answers on and none that a client should use. Falling back to the bind
  // address is right on a single-homed host and honest everywhere else: what comes out
  // is what the node was told, and an operator who sees 0.0.0.0 here knows why a client
  // could not use it.
  const auto advertised = [this](const std::string& bind_address) { return _config->sip_public_address.empty() ? bind_address : _config->sip_public_address; };

  const auto add = [&transports, &advertised](const std::string& transport, const std::string& bind_address, std::uint16_t port, bool secure) {
    boost::json::object entry;
    entry["transport"] = transport;
    entry["address"] = advertised(bind_address);
    entry["port"] = port;
    entry["uri"] = std::string(secure ? "sips:" : "sip:") + advertised(bind_address) + ":" + std::to_string(port) + ";transport=" + transport;

    transports.push_back(std::move(entry));
  };

  if (_config->udp_enable) add("udp", _config->udp_address, _config->udp_port, false);
  if (_config->tcp_enable) add("tcp", _config->tcp_address, _config->tcp_port, false);
  if (_config->tls_enable) add("tls", _config->tls_address, _config->tls_port, true);

  if (_config->websocket_enable) {
    const bool secure = _config->websocket_tls;
    add(secure ? "wss" : "ws", _config->websocket_address, _config->websocket_port, secure);
  }

  return transports;
}

// Shared

void ProvisioningAPI::_with_realm(const std::string& realm_name, RouteContext context, std::function<void(std::shared_ptr<types::Realm>, RouteContext)> then) {
  _datastore->realm_get_by_name(_executor, realm_name, [context, then, realm_name](plugins::Result<std::shared_ptr<types::Realm>> result) mutable {
    if (!result.ok) {
      write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
      return context.done();
    }

    if (!result.value) {
      write_error(context.response, http::status::not_found, "not_found", "no such realm: " + realm_name);
      return context.done();
    }

    then(result.value, std::move(context));
  });
}

void ProvisioningAPI::_fail(RouteContext context, const plugins::Status& status, std::string code, std::string message, http::status http_status) {
  _logger->debug("provisioning refused: " + status.error);

  write_error(context.response, http_status, std::move(code), std::move(message));
  context.done();
}

boost::json::object ProvisioningAPI::_realm_json(const types::Realm& realm) {
  boost::json::object object;
  object["name"] = realm.name;
  object["id"] = realm.id;
  object["nonce_expiry"] = realm.nonce_expiry;
  object["registration_timeout"] = realm.registration_timeout;
  object["registration_minimum"] = realm.registration_minimum;
  object["media_anchor"] = realm.media.anchor;
  object["media_profiles"] = types::MediaPolicy::to_string(realm.media.profiles);

  // nonce_secret is deliberately absent. It is the key this node mints nonces with, and
  // an API that hands it back is an API that leaks it into every log that records a
  // response.
  return object;
}

boost::json::object ProvisioningAPI::_account_json(const types::Account& account) {
  boost::json::object object;
  object["id"] = account.id;

  if (account.identity && account.identity->uri) {
    object["uri"] = account.identity->uri->to_string();
    object["user"] = account.identity->uri->user;
    object["realm"] = account.identity->uri->host;
  }

  // ha1 is the password in the only form this server holds it. It does not come back
  // out.
  return object;
}

boost::json::object ProvisioningAPI::_location_json(const types::Location& location, const std::string& uri) {
  boost::json::object object;
  object["account"] = uri;
  object["account_id"] = location.account_id;
  object["contact"] = location.contact ? location.contact->to_string() : "";
  object["registered_at"] = static_cast<std::int64_t>(location.registered_at);
  object["expires_at"] = static_cast<std::int64_t>(location.expires_at);
  object["nat"] = location.nat;

  // Empty on a single node, and the whole point on more than one.
  object["node_id"] = location.node_id;
  object["flow_id"] = location.flow_id;
  object["path"] = location.path;

  return object;
}

}  // namespace athenasip::api
