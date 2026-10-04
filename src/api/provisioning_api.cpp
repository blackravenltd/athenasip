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
#include "../types/turn_credential.h"
#include "../util.h"
#include "api_json.h"

namespace athenasip::api {

namespace {

// A subscriber is a user within a realm, which is a SIP URI. The path carries the two
// separately; this joins them.
std::shared_ptr<types::SIPIdentity> identity_of(const std::string& realm_name, const std::string& user) {
  return std::make_shared<types::SIPIdentity>("sip:" + user + "@" + realm_name);
}

std::string uri_of(const std::string& realm_name, const std::string& user) { return "sip:" + user + "@" + realm_name; }

// HA1 (RFC 2617 / RFC 7616): the hash of user, realm and password, and the only form of
// the password this server holds.
std::string ha1_of(const std::string& user, const std::string& realm_name, const std::string& password) {
  return Util::to_lower(Util::md5(user + ":" + realm_name + ":" + password));
}

// The SHA-256 HA1 (RFC 8760). Both are computed while the password is in hand, since
// neither can be derived from the other; a subscriber imported as a bare MD5 hash can
// answer only MD5 challenges.
std::string ha1_sha256_of(const std::string& user, const std::string& realm_name, const std::string& password) {
  return Util::to_lower(Util::sha256(user + ":" + realm_name + ":" + password));
}

// Reads a realm's behaviour section: what it does differently from the server's default.
// Only what was given is changed, and a setting given as null goes back to inheriting.
// Returns empty on success, otherwise what was wrong.
std::string read_behaviour(const boost::json::object& body, types::Behaviour& behaviour) {
  // These belong in the behaviour section; at the top level they are refused, not ignored.
  for (const auto* moved : {"media_anchor", "media_profiles"}) {
    if (body.contains(moved)) return std::string(moved) + " has moved into the behaviour section";
  }

  const auto* section = body.if_contains("behaviour");
  if (section == nullptr) return "";
  if (!section->is_object()) return "behaviour is an object";

  for (const auto& [key, value] : section->as_object()) {
    if (key == "media_anchor") {
      if (value.is_null()) {
        behaviour.media_anchor.reset();
      } else if (value.is_bool()) {
        behaviour.media_anchor = value.as_bool();
      } else {
        return "behaviour.media_anchor is true, false or null";
      }
    } else if (key == "media_profile") {
      if (value.is_null()) {
        behaviour.media_profile.reset();
      } else if (const auto profiles = value.is_string() ? types::MediaPolicy::parse_profiles(std::string(value.as_string())) : std::nullopt) {
        behaviour.media_profile = *profiles;
      } else {
        return "behaviour.media_profile is mirror, transport, webrtc, rtp, srtp or null";
      }
    } else if (key == "rewrite_contact") {
      if (value.is_null()) {
        behaviour.rewrite_contact.reset();
      } else if (value.is_bool()) {
        behaviour.rewrite_contact = value.as_bool();
      } else {
        return "behaviour.rewrite_contact is true, false or null";
      }
    } else if (key == "qualify_interval") {
      if (value.is_null()) {
        behaviour.qualify_interval.reset();
      } else if (value.is_int64() && types::Behaviour::valid_qualify_interval(value.as_int64())) {
        behaviour.qualify_interval = static_cast<std::uint32_t>(value.as_int64());
      } else {
        return "behaviour.qualify_interval is 0 for never, seconds from " + std::to_string(types::Behaviour::kQualifyMinimum) + " to " +
               std::to_string(types::Behaviour::kQualifyMaximum) + ", or null";
      }
    } else {
      return "behaviour has no setting called " + std::string(key);
    }
  }

  return "";
}

// A subscriber's behaviour section has one setting: what its endpoint is. Anchoring is a
// realm's choice.
std::string read_behaviour(const boost::json::object& body, types::Subscriber& subscriber) {
  const auto* section = body.if_contains("behaviour");
  if (section == nullptr) return "";
  if (!section->is_object()) return "behaviour is an object";

  for (const auto& [key, value] : section->as_object()) {
    if (key != "media_profile") return "a subscriber's behaviour has no setting called " + std::string(key);

    if (value.is_null()) {
      subscriber.media_profile.reset();
    } else if (const auto profiles = value.is_string() ? types::MediaPolicy::parse_profiles(std::string(value.as_string())) : std::nullopt) {
      subscriber.media_profile = *profiles;
    } else {
      return "behaviour.media_profile is mirror, transport, webrtc, rtp, srtp or null";
    }
  }

  return "";
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

  using namespace types::roles;

  // Reading realms admits either role, so that somebody who only manages subscribers can
  // pick a realm. Changing one needs manage-realms.
  const std::vector<std::string> read_realms = {manage_realms, manage_realm_subscribers};

  // Open, for healthchecks and load balancers. It says the node is up and what it is.
  router.add_open(http::verb::get, "/api/v1/health", [self](RouteContext c) { self->_health(std::move(c)); });

  // Where the realm is served from; it says nothing about who is on it, hence the
  // cluster-status role.
  router.add(http::verb::get, "/api/v1/nodes", {view_cluster_status}, [self](RouteContext c) { self->_node_list(std::move(c)); });

  router.add(http::verb::get, "/api/v1/realms", read_realms, [self](RouteContext c) { self->_realm_list(std::move(c)); });
  router.add(http::verb::post, "/api/v1/realms", {manage_realms}, [self](RouteContext c) { self->_realm_create(std::move(c)); });
  router.add(http::verb::get, "/api/v1/realms/{realm}", read_realms, [self](RouteContext c) { self->_realm_get(std::move(c)); });
  router.add(http::verb::put, "/api/v1/realms/{realm}", {manage_realms}, [self](RouteContext c) { self->_realm_update(std::move(c)); });
  router.add(http::verb::delete_, "/api/v1/realms/{realm}", {manage_realms}, [self](RouteContext c) { self->_realm_delete(std::move(c)); });

  router.add(http::verb::get, "/api/v1/realms/{realm}/subscribers", {manage_realm_subscribers},
             [self](RouteContext c) { self->_subscriber_list(std::move(c)); });
  router.add(http::verb::post, "/api/v1/realms/{realm}/subscribers", {manage_realm_subscribers},
             [self](RouteContext c) { self->_subscriber_create(std::move(c)); });
  router.add(http::verb::get, "/api/v1/realms/{realm}/subscribers/{user}", {manage_realm_subscribers},
             [self](RouteContext c) { self->_subscriber_get(std::move(c)); });
  router.add(http::verb::put, "/api/v1/realms/{realm}/subscribers/{user}", {manage_realm_subscribers},
             [self](RouteContext c) { self->_subscriber_update(std::move(c)); });
  router.add(http::verb::delete_, "/api/v1/realms/{realm}/subscribers/{user}", {manage_realm_subscribers},
             [self](RouteContext c) { self->_subscriber_delete(std::move(c)); });

  // Reads registrations and provisions nothing, so it takes the status role.
  router.add(http::verb::get, "/api/v1/registrations", {view_cluster_status}, [self](RouteContext c) { self->_registration_list(std::move(c)); });

  // The same role as /nodes: how to reach the realm, not who is on it.
  router.add(http::verb::get, "/api/v1/client/config", {view_cluster_status}, [self](RouteContext c) { self->_client_config(std::move(c)); });
}

// Realms

void ProvisioningAPI::_realm_list(RouteContext context) {
  auto self = shared_from_this();
  _datastore->realm_list(_executor, [self, context](plugins::Result<std::vector<std::shared_ptr<types::Realm>>> result) mutable {
    if (!result.ok) {
      write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
      return context.done();
    }

    boost::json::array realms;
    for (const auto& realm : result.value) {
      if (realm) realms.push_back(self->_realm_json(*realm));
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

  // Generated when not given: a realm needs a secret to mint nonces. It is never returned.
  realm->nonce_secret = string_field(*body, "nonce_secret").value_or(Util::generate_random_string("", 32));

  if (const auto expiry = uint_field(*body, "nonce_expiry")) realm->nonce_expiry = *expiry;
  if (const auto timeout = uint_field(*body, "registration_timeout")) realm->registration_timeout = *timeout;
  if (const auto minimum = uint_field(*body, "registration_minimum")) realm->registration_minimum = *minimum;

  if (const auto refused = read_behaviour(*body, realm->behaviour); !refused.empty()) {
    write_error(context.response, http::status::bad_request, "invalid_request", refused);
    return context.done();
  }

  auto self = shared_from_this();
  _datastore->realm_create(_executor, realm, [self, context, realm](plugins::Status status) mutable {
    if (!status.ok) {
      return self->_fail(std::move(context), status, "conflict", "realm " + realm->name + " already exists", http::status::conflict);
    }

    write_json(context.response, http::status::created, self->_realm_json(*realm));
    context.done();
  });
}

void ProvisioningAPI::_realm_get(RouteContext context) {
  const auto name = context.parameter("realm");

  auto self = shared_from_this();
  _with_realm(name, std::move(context), [self](std::shared_ptr<types::Realm> realm, RouteContext context) {
    write_json(context.response, http::status::ok, self->_realm_json(*realm));
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
    // Only what was given is changed; an omitted field keeps its value.
    if (const auto secret = string_field(*body, "nonce_secret")) realm->nonce_secret = *secret;
    if (const auto expiry = uint_field(*body, "nonce_expiry")) realm->nonce_expiry = *expiry;
    if (const auto timeout = uint_field(*body, "registration_timeout")) realm->registration_timeout = *timeout;
    if (const auto minimum = uint_field(*body, "registration_minimum")) realm->registration_minimum = *minimum;

    if (const auto refused = read_behaviour(*body, realm->behaviour); !refused.empty()) {
      write_error(context.response, http::status::bad_request, "invalid_request", refused);
      return context.done();
    }

    self->_datastore->realm_update(self->_executor, realm, [self, context, realm](plugins::Status status) mutable {
      if (!status.ok) {
        write_error(context.response, http::status::internal_server_error, "datastore_error", status.error);
        return context.done();
      }

      write_json(context.response, http::status::ok, self->_realm_json(*realm));
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

// Subscribers

void ProvisioningAPI::_subscriber_list(RouteContext context) {
  const auto realm_name = context.parameter("realm");

  auto self = shared_from_this();

  _with_realm(realm_name, std::move(context), [self](std::shared_ptr<types::Realm> realm, RouteContext context) {
    self->_datastore->subscriber_list(self->_executor, realm->name, [context](plugins::Result<std::vector<std::shared_ptr<types::Subscriber>>> result) mutable {
      if (!result.ok) {
        write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
        return context.done();
      }

      boost::json::array subscribers;
      for (const auto& subscriber : result.value) {
        if (subscriber) subscribers.push_back(_subscriber_json(*subscriber));
      }

      write_json(context.response, http::status::ok, subscribers);
      context.done();
    });
  });
}

void ProvisioningAPI::_subscriber_create(RouteContext context) {
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

    // ha1 is for importing subscribers from a system that already holds one. One of the two
    // is required, or the subscriber could never authenticate.
    if (!password && !ha1) {
      write_error(context.response, http::status::bad_request, "invalid_request", "password or ha1 is required");
      return context.done();
    }

    auto subscriber = std::make_shared<types::Subscriber>();
    subscriber->identity = identity_of(realm->name, *user);
    subscriber->id = Util::stable_id(uri_of(realm->name, *user));

    if (password) {
      subscriber->ha1 = ha1_of(*user, realm->name, *password);
      subscriber->ha1_sha256 = ha1_sha256_of(*user, realm->name, *password);
    }

    // An explicit hash wins over one derived from a password.
    if (ha1) subscriber->ha1 = *ha1;
    if (const auto imported = string_field(*body, "ha1_sha256")) subscriber->ha1_sha256 = *imported;

    if (const auto why = read_behaviour(*body, *subscriber); !why.empty()) {
      write_error(context.response, http::status::bad_request, "invalid_request", why);
      return context.done();
    }

    self->_datastore->subscriber_create(self->_executor, subscriber, [self, context, subscriber](plugins::Status status) mutable {
      if (!status.ok) {
        return self->_fail(std::move(context), status, "conflict", "that subscriber already exists", http::status::conflict);
      }

      write_json(context.response, http::status::created, _subscriber_json(*subscriber));
      context.done();
    });
  });
}

void ProvisioningAPI::_subscriber_get(RouteContext context) {
  const auto realm_name = context.parameter("realm");
  const auto user = context.parameter("user");

  _datastore->subscriber_get(_executor, identity_of(realm_name, user), [context](plugins::Result<std::shared_ptr<types::Subscriber>> result) mutable {
    if (!result.ok) {
      write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
      return context.done();
    }

    if (!result.value) {
      write_error(context.response, http::status::not_found, "not_found", "no such subscriber");
      return context.done();
    }

    write_json(context.response, http::status::ok, _subscriber_json(*result.value));
    context.done();
  });
}

void ProvisioningAPI::_subscriber_update(RouteContext context) {
  const auto body = parse_object(context.body);
  if (!body) {
    write_error(context.response, http::status::bad_request, "invalid_json", "the body is not a JSON object");
    return context.done();
  }

  const auto realm_name = context.parameter("realm");
  const auto user = context.parameter("user");

  auto self = shared_from_this();
  _datastore->subscriber_get(_executor, identity_of(realm_name, user),
                             [self, context, body, realm_name, user](plugins::Result<std::shared_ptr<types::Subscriber>> result) mutable {
                               if (!result.ok) {
                                 write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
                                 return context.done();
                               }

                               if (!result.value) {
                                 write_error(context.response, http::status::not_found, "not_found", "no such subscriber");
                                 return context.done();
                               }

                               auto subscriber = result.value;

                               if (const auto password = string_field(*body, "password")) {
                                 subscriber->ha1 = ha1_of(user, realm_name, *password);
                                 subscriber->ha1_sha256 = ha1_sha256_of(user, realm_name, *password);
                               }

                               if (const auto ha1 = string_field(*body, "ha1")) subscriber->ha1 = *ha1;
                               if (const auto sha256 = string_field(*body, "ha1_sha256")) subscriber->ha1_sha256 = *sha256;

                               // Read before anything is written, so a refusal changes nothing.
                               if (const auto why = read_behaviour(*body, *subscriber); !why.empty()) {
                                 write_error(context.response, http::status::bad_request, "invalid_request", why);
                                 return context.done();
                               }

                               self->_datastore->subscriber_update(self->_executor, subscriber, [context, subscriber](plugins::Status status) mutable {
                                 if (!status.ok) {
                                   write_error(context.response, http::status::internal_server_error, "datastore_error", status.error);
                                   return context.done();
                                 }

                                 write_json(context.response, http::status::ok, _subscriber_json(*subscriber));
                                 context.done();
                               });
                             });
}

void ProvisioningAPI::_subscriber_delete(RouteContext context) {
  const auto realm_name = context.parameter("realm");
  const auto user = context.parameter("user");

  auto self = shared_from_this();
  auto identity = identity_of(realm_name, user);

  // The store fails a delete of a subscriber that is not there; the read first makes that a
  // 404 rather than a 500.
  _datastore->subscriber_get(_executor, identity, [self, context, identity](plugins::Result<std::shared_ptr<types::Subscriber>> result) mutable {
    if (!result.ok) {
      write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
      return context.done();
    }

    if (!result.value) {
      write_error(context.response, http::status::not_found, "not_found", "no such subscriber");
      return context.done();
    }

    self->_datastore->subscriber_delete(self->_executor, identity, [context](plugins::Status status) mutable {
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

  // One realm when asked for one, every realm otherwise. The walk costs a read per
  // subscriber; this is a diagnostic, not on the call path.
  const auto realm_filter = context.query.find("realm");
  const auto wanted = realm_filter == context.query.end() ? std::string() : realm_filter->second;

  auto registrations = std::make_shared<boost::json::array>();

  auto finish = [context, registrations]() mutable {
    write_json(context.response, http::status::ok, *registrations);
    context.done();
  };

  // Each step starts the next from its own completion.
  auto walk_subscribers = [self, registrations](std::vector<std::shared_ptr<types::Subscriber>> subscribers, std::function<void()> done) {
    struct Walk : std::enable_shared_from_this<Walk> {
      std::shared_ptr<ProvisioningAPI> api;
      std::shared_ptr<boost::json::array> out;
      std::vector<std::shared_ptr<types::Subscriber>> subscribers;
      std::function<void()> done;
      std::size_t index = 0;

      void step() {
        if (index >= subscribers.size()) return done();

        auto subscriber = subscribers[index++];
        if (!subscriber || !subscriber->identity || !subscriber->identity->uri) return step();

        auto self = shared_from_this();
        const auto uri = subscriber->identity->uri->to_string();

        api->_datastore->location_list(api->_executor, subscriber->id, [self, uri](plugins::Result<std::vector<types::Location>> result) mutable {
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
    walk->subscribers = std::move(subscribers);
    walk->done = std::move(done);
    walk->step();
  };

  if (!wanted.empty()) {
    _datastore->subscriber_list(_executor, wanted,
                                [context, walk_subscribers, finish](plugins::Result<std::vector<std::shared_ptr<types::Subscriber>>> result) mutable {
                                  if (!result.ok) {
                                    write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
                                    return context.done();
                                  }

                                  walk_subscribers(std::move(result.value), finish);
                                });
    return;
  }

  _datastore->realm_list(_executor, [self, context, walk_subscribers, finish](plugins::Result<std::vector<std::shared_ptr<types::Realm>>> result) mutable {
    if (!result.ok) {
      write_error(context.response, http::status::internal_server_error, "datastore_error", result.error);
      return context.done();
    }

    auto realms = std::make_shared<std::vector<std::shared_ptr<types::Realm>>>(std::move(result.value));
    auto index = std::make_shared<std::size_t>(0);

    auto next = std::make_shared<std::function<void()>>();
    *next = [self, realms, index, walk_subscribers, finish, next]() mutable {
      if (*index >= realms->size()) return finish();

      auto realm = (*realms)[(*index)++];
      if (!realm) return (*next)();

      self->_datastore->subscriber_list(self->_executor, realm->name,
                                        [walk_subscribers, next](plugins::Result<std::vector<std::shared_ptr<types::Subscriber>>> subscribers) mutable {
                                          if (!subscribers.ok) return (*next)();

                                          walk_subscribers(std::move(subscribers.value), [next]() { (*next)(); });
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

void ProvisioningAPI::_client_config(RouteContext context) {
  boost::json::object out;

  // Every transport this node serves, the same list /nodes gives.
  out["transports"] = _local_transports();

  // The one a browser can use. Absent when there is no secure WebSocket listener: a page
  // served over https cannot use ws://.
  for (const auto& transport : _local_transports()) {
    if (transport.at("transport").as_string() == "wss") {
      out["websocket_uri"] =
          "wss://" + std::string(transport.at("address").as_string()) + ":" + std::to_string(transport.at("port").to_number<std::uint64_t>());
      break;
    }
  }

  // Failover targets: this node first, then the others that are up and current, with their
  // secure WebSocket URIs in the same order.
  auto nodes = _nodes_json(true);
  boost::json::array websocket_uris;
  for (const auto& node : nodes) {
    // Another node's entries came off the bus: skip any that is not shaped right.
    const auto* transports = node.as_object().if_contains("transports");
    if (transports == nullptr || !transports->is_array()) continue;

    for (const auto& transport : transports->as_array()) {
      if (!transport.is_object()) continue;
      const auto* kind = transport.as_object().if_contains("transport");
      const auto* address = transport.as_object().if_contains("address");
      const auto* port = transport.as_object().if_contains("port");
      if (kind == nullptr || address == nullptr || port == nullptr || !kind->is_string() || !address->is_string() || !port->is_number()) continue;
      if (kind->as_string() != "wss") continue;

      websocket_uris.push_back(boost::json::string("wss://" + std::string(address->as_string()) + ":" + std::to_string(port->to_number<std::uint64_t>())));
    }
  }
  out["nodes"] = std::move(nodes);
  out["websocket_uris"] = std::move(websocket_uris);

  // Shaped as RTCIceServer, so a browser can hand it to RTCPeerConnection unchanged.
  // TURN credentials are minted per request and expire; a stun: URL gets none.
  boost::json::array ice;
  const auto now = std::time(nullptr);

  for (const auto& server : _config->ice_servers) {
    boost::json::object entry;
    entry["urls"] = server.url;

    const bool needs_credential = server.url.rfind("turn:", 0) == 0 || server.url.rfind("turns:", 0) == 0;

    if (needs_credential) {
      // The caller's name, so a relay session can be traced in the TURN server's log. It
      // must be a plain key, not Caller::describe(): coturn answers 400 to prose here.
      const auto asked_by = context.caller.user ? context.caller.user->key() : std::string("anonymous");

      const auto credential = types::TurnCredential::issue(_config->turn_shared_secret, asked_by, now, _config->turn_credential_ttl);

      if (!credential.username.empty()) {
        entry["username"] = credential.username;
        entry["credential"] = credential.password;
        entry["expires_at"] = static_cast<std::int64_t>(credential.expires_at);
      }
    }

    ice.push_back(std::move(entry));
  }

  out["ice_servers"] = std::move(ice);

  write_json(context.response, http::status::ok, out);
  context.done();
}

boost::json::array ProvisioningAPI::_nodes_json(bool usable_only) const {
  boost::json::object node;
  node["id"] = _config->sip_node_id;

  // A list with no entry marked self did not come from a node.
  node["self"] = true;
  node["status"] = "ok";
  node["version"] = _version;
  node["transports"] = _local_transports();

  boost::json::array nodes;
  nodes.push_back(std::move(node));

  // The others, as each last described itself on the bus. After three missed heartbeats a
  // node is listed but marked stale, so nobody routes to it on an old "ok".
  if (_nodes) {
    for (const auto& other : _nodes->list(_node_heartbeat * 3)) {
      if (other.id == _config->sip_node_id) continue;
      if (usable_only && (other.stale || other.status != "ok")) continue;

      boost::json::object entry;
      entry["id"] = other.id;
      entry["self"] = false;
      entry["status"] = other.status;
      entry["stale"] = other.stale;
      entry["version"] = other.version;
      entry["at"] = other.at;
      entry["transports"] = other.transports;

      // For administrators; a client has no use for the inter-node listener.
      if (!usable_only && !other.cluster_address.empty()) {
        boost::json::object peer;
        peer["address"] = other.cluster_address;
        peer["port"] = other.cluster_port;
        entry["cluster"] = std::move(peer);
      }
      nodes.push_back(std::move(entry));
    }
  }

  return nodes;
}

void ProvisioningAPI::_node_list(RouteContext context) {
  write_json(context.response, http::status::ok, _nodes_json(false));
  context.done();
}

boost::json::array ProvisioningAPI::_local_transports() const {
  boost::json::array transports;

  for (const auto& advertised : _config->advertised_transports()) {
    boost::json::object entry;
    entry["transport"] = advertised.transport;
    entry["address"] = advertised.address;
    entry["port"] = advertised.port;
    entry["uri"] = advertised.uri();
    transports.push_back(std::move(entry));
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

boost::json::object ProvisioningAPI::_realm_json(const types::Realm& realm) const {
  boost::json::object object;
  object["name"] = realm.name;
  object["id"] = realm.id;
  object["nonce_expiry"] = realm.nonce_expiry;
  object["registration_timeout"] = realm.registration_timeout;
  object["registration_minimum"] = realm.registration_minimum;
  // What the realm chose (null where it inherits), and the effective result over the
  // server's default.
  boost::json::object chosen;
  chosen["media_anchor"] = realm.behaviour.media_anchor ? boost::json::value(*realm.behaviour.media_anchor) : boost::json::value(nullptr);
  chosen["media_profile"] =
      realm.behaviour.media_profile ? boost::json::value(types::MediaPolicy::to_string(*realm.behaviour.media_profile)) : boost::json::value(nullptr);
  chosen["rewrite_contact"] = realm.behaviour.rewrite_contact ? boost::json::value(*realm.behaviour.rewrite_contact) : boost::json::value(nullptr);
  chosen["qualify_interval"] = realm.behaviour.qualify_interval ? boost::json::value(*realm.behaviour.qualify_interval) : boost::json::value(nullptr);
  object["behaviour"] = std::move(chosen);

  const auto policy_json = [](const types::MediaPolicy& policy, std::uint32_t qualify_interval, bool rewrite_contact) {
    boost::json::object json;
    json["media_anchor"] = policy.anchor;
    json["media_profile"] = types::MediaPolicy::to_string(policy.profiles);
    json["qualify_interval"] = qualify_interval;
    json["rewrite_contact"] = rewrite_contact;
    return json;
  };
  object["behaviour_effective"] = policy_json(realm.behaviour.over(_config->behaviour), realm.behaviour.qualify_over(_config->behaviour_qualify_interval),
                                              realm.behaviour.rewrite_contact.value_or(_config->behaviour_rewrite_contact));
  // The server's default on its own: what "inherit" would give.
  object["behaviour_default"] = policy_json(_config->behaviour, _config->behaviour_qualify_interval, _config->behaviour_rewrite_contact);

  // nonce_secret is never returned.
  return object;
}

boost::json::object ProvisioningAPI::_subscriber_json(const types::Subscriber& subscriber) {
  boost::json::object object;
  object["id"] = subscriber.id;

  if (subscriber.identity && subscriber.identity->uri) {
    object["uri"] = subscriber.identity->uri->to_string();
    object["user"] = subscriber.identity->uri->user;
    object["realm"] = subscriber.identity->uri->host;
  }

  // What the subscriber chose; null where it takes its realm's.
  boost::json::object behaviour;
  behaviour["media_profile"] =
      subscriber.media_profile ? boost::json::value(types::MediaPolicy::to_string(*subscriber.media_profile)) : boost::json::value(nullptr);
  object["behaviour"] = std::move(behaviour);

  // ha1 is never returned.
  return object;
}

boost::json::object ProvisioningAPI::_location_json(const types::Location& location, const std::string& uri) {
  boost::json::object object;
  object["subscriber"] = uri;
  object["subscriber_id"] = location.subscriber_id;
  object["contact"] = location.contact ? location.contact->to_string() : "";
  object["registered_at"] = static_cast<std::int64_t>(location.registered_at);
  object["expires_at"] = static_cast<std::int64_t>(location.expires_at);
  object["nat"] = location.nat;

  // Empty on a single node.
  object["node_id"] = location.node_id;
  object["flow_id"] = location.flow_id;
  object["path"] = location.path;

  return object;
}

}  // namespace athenasip::api
