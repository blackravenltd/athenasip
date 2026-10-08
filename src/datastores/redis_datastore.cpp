//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "redis_datastore.h"

#include <boost/asio/consign.hpp>
#include <boost/asio/detached.hpp>
#include <boost/json.hpp>
#include <boost/redis/src.hpp>
#include <boost/system/system_error.hpp>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "../config.h"

namespace athenasip::datastores {
namespace {

RedisError make_redis_response_error(const std::string& diagnostic) {
  return RedisError(boost::system::errc::make_error_code(boost::system::errc::protocol_error), diagnostic);
}

std::string json_string(const boost::json::object& obj, const char* key) {
  const auto* value = obj.if_contains(key);
  if (value == nullptr || !value->is_string()) {
    throw std::runtime_error(std::string("Missing or non-string JSON field: ") + key);
  }

  return std::string(value->as_string().c_str());
}

std::uint64_t json_uint64(const boost::json::object& obj, const char* key) {
  const auto* value = obj.if_contains(key);
  if (value == nullptr) {
    throw std::runtime_error(std::string("Missing JSON field: ") + key);
  }

  if (value->is_uint64()) {
    return value->as_uint64();
  }

  if (value->is_int64()) {
    const auto signed_value = value->as_int64();
    if (signed_value < 0) {
      throw std::runtime_error(std::string("Negative value for unsigned JSON field: ") + key);
    }
    return static_cast<std::uint64_t>(signed_value);
  }

  if (value->is_string()) {
    return static_cast<std::uint64_t>(std::stoull(std::string(value->as_string().c_str())));
  }

  throw std::runtime_error(std::string("Non-integer JSON field: ") + key);
}

std::uint32_t json_uint32(const boost::json::object& obj, const char* key) {
  const auto value = json_uint64(obj, key);
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error(std::string("JSON field out of uint32 range: ") + key);
  }
  return static_cast<std::uint32_t>(value);
}

std::uint16_t parse_port(std::string value) {
  if (value.empty()) {
    return 6379;
  }

  const auto parsed = std::stoul(value);
  if (parsed > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("Redis URL port is out of range: " + value);
  }

  return static_cast<std::uint16_t>(parsed);
}

types::Location parse_location(const std::string& value) {
  const auto parsed = boost::json::parse(value);
  const auto& obj = parsed.as_object();

  types::Location location;
  location.subscriber_id = json_uint64(obj, "subscriber_id");
  location.contact = std::make_shared<types::SIPUri>(json_string(obj, "contact"));
  location.registered_at = static_cast<std::time_t>(json_uint64(obj, "registered_at"));
  location.expires_at = static_cast<std::time_t>(json_uint64(obj, "expires_at"));
  location.nat = json_string(obj, "nat") == "Y";

  if (obj.if_contains("node_id")) location.node_id = json_string(obj, "node_id");
  if (obj.if_contains("flow_id")) location.flow_id = json_string(obj, "flow_id");
  if (obj.if_contains("path")) location.path = json_string(obj, "path");
  if (obj.if_contains("instance")) location.instance = json_string(obj, "instance");
  if (const auto* reg_id = obj.if_contains("reg_id"); reg_id != nullptr && reg_id->is_number()) location.reg_id = reg_id->to_number<std::uint32_t>();
  if (const auto* push = obj.if_contains("push"); push != nullptr && push->is_bool()) location.push = push->as_bool();

  return location;
}

// A list is a set read followed by a read per member. With nothing to block on, each
// step starts the next from its own completion; done() runs at the end.
using SequenceStep = std::function<void(std::string, std::function<void()>)>;

void run_sequence(std::shared_ptr<std::vector<std::string>> items, std::size_t index, std::shared_ptr<SequenceStep> step,
                  std::shared_ptr<std::function<void()>> done) {
  if (index >= items->size()) {
    if (*done) (*done)();
    return;
  }

  const auto item = (*items)[index];
  (*step)(item, [items, index, step, done]() { run_sequence(items, index + 1, step, done); });
}

void run_sequence(std::vector<std::string> items, SequenceStep step, std::function<void()> done) {
  run_sequence(std::make_shared<std::vector<std::string>>(std::move(items)), 0, std::make_shared<SequenceStep>(std::move(step)),
               std::make_shared<std::function<void()>>(std::move(done)));
}

}  // namespace

RedisDatastore::RedisDatastore(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<types::URL> url)
    : _logger(std::make_shared<loggers::LoggerScoped>("redis_datastore", std::move(logger))), _url(std::move(url)) {
  _apply_url(_url);
}

RedisDatastore::~RedisDatastore() { close(); }

std::string RedisDatastore::name() const { return "redis"; }

std::string RedisDatastore::version() const { return "0.0.1"; }

void RedisDatastore::close() {
  if (!_started.exchange(false)) {
    return;
  }

  if (_connection) {
    _connection->cancel();
  }

  _work_guard.reset();
  _io_context.stop();

  if (_io_thread.joinable()) {
    _io_thread.join();
  }

  _connection.reset();
  _io_context.restart();
}

bool RedisDatastore::is_connected() const { return _started.load(); }

void RedisDatastore::connect(plugins::Executor on, plugins::StatusHandler handler) {
  if (_started.load()) {
    return _async_ping([this, on, handler](RedisError error, bool ok) mutable {
      _complete(on, handler, !error && ok ? plugins::Status::success() : plugins::Status::failure(error ? error.message() : "PING failed"));
    });
  }

  _logger->debug("URL: " + _host + ":" + std::to_string(_port));

  try {
    _work_guard = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(_io_context.get_executor());
    // Routes the library's logging through this node's logger: its info is this node's
    // debug, warnings and worse keep their level. The logger is captured by value because
    // the library calls it from the IO thread, possibly after this object begins to close.
    using Level = boost::redis::logger::level;

    auto logger = _logger;
    boost::redis::logger forward(Level::debug, [logger](Level level, std::string_view message) {
      const auto line = "connection: " + std::string(message);

      if (level <= Level::err) return logger->error(line);
      if (level == Level::warning) return logger->warn(line);
      logger->debug(line);
    });

    _connection = std::make_shared<boost::redis::connection>(_io_context.get_executor(), std::move(forward));

    auto cfg = _make_config();
    _connection->async_run(cfg, boost::asio::consign(boost::asio::detached, _connection));

    _io_thread = std::thread([this]() {
      try {
        _io_context.run();
      } catch (const std::exception& ex) {
        _logger->error(std::string("IO thread exception: ") + ex.what());
      } catch (...) {
        _logger->error("IO thread unknown exception.");
      }
    });

    _started.store(true);
  } catch (const std::exception& ex) {
    _logger->error(std::string("Exception while connecting: ") + ex.what());
    close();
    return _complete(on, handler, plugins::Status::failure(ex.what()));
  } catch (...) {
    _logger->error("Unknown exception occurred while connecting.");
    close();
    return _complete(on, handler, plugins::Status::failure("unknown exception while connecting"));
  }

  // connect() succeeds only once Redis has answered a PING.
  _async_ping([this, on, handler](RedisError error, bool ok) mutable {
    if (error || !ok) {
      _logger->error("Initial PING failed.");
      close();
      return _complete(on, handler, plugins::Status::failure(error ? error.message() : "initial PING failed"));
    }

    _complete(on, handler, plugins::Status::success());
  });
}

void RedisDatastore::realm_get_by_name(plugins::Executor on, std::string realm_name, plugins::Handler<std::shared_ptr<types::Realm>> handler) {
  using Answer = plugins::Result<std::shared_ptr<types::Realm>>;

  _async_get(_realm_key(realm_name), [this, on, handler](RedisError error, std::optional<std::string> value) mutable {
    if (error) return _complete(on, handler, Answer::failure(error.message()));

    // Not found is a success carrying nothing.
    if (!value) return _complete(on, handler, Answer::success(nullptr));

    try {
      _complete(on, handler, Answer::success(_parse_realm(*value)));
    } catch (const std::exception& ex) {
      _logger->error("realm_get_by_name: " + std::string(ex.what()));
      _complete(on, handler, Answer::failure(ex.what()));
    }
  });
}

void RedisDatastore::realm_create(plugins::Executor on, std::shared_ptr<types::Realm> realm, plugins::StatusHandler handler) {
  if (!realm || realm->name.empty()) return _complete(on, handler, plugins::Status::failure("realm_create: no realm"));

  const auto key = _realm_key(realm->name);
  const auto index = _realm_index_key();
  const auto name = realm->name;
  const auto body = _serialise_realm(realm);

  // An existing realm is a conflict, not an overwrite.
  _async_exists(key, [this, on, handler, key, index, name, body](RedisError error, bool exists) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
    if (exists) return _complete(on, handler, plugins::Status::failure("realm_create: " + name + " already exists"));

    _async_set(key, body, [this, on, handler, index, name](RedisError error, bool ok) mutable {
      if (error || !ok) return _complete(on, handler, plugins::Status::failure(error ? error.message() : "realm_create: SET failed"));

      _async_sadd(index, name, [this, on, handler](RedisError error, bool) mutable {
        _complete(on, handler, error ? plugins::Status::failure(error.message()) : plugins::Status::success());
      });
    });
  });
}

void RedisDatastore::realm_update(plugins::Executor on, std::shared_ptr<types::Realm> realm, plugins::StatusHandler handler) {
  if (!realm || realm->name.empty()) return _complete(on, handler, plugins::Status::failure("realm_update: no realm"));

  const auto key = _realm_key(realm->name);
  const auto name = realm->name;
  const auto body = _serialise_realm(realm);

  _async_exists(key, [this, on, handler, key, name, body](RedisError error, bool exists) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
    if (!exists) return _complete(on, handler, plugins::Status::failure("realm_update: " + name + " does not exist"));

    _async_set(key, body, [this, on, handler](RedisError error, bool ok) mutable {
      _complete(on, handler, !error && ok ? plugins::Status::success() : plugins::Status::failure(error ? error.message() : "realm_update: SET failed"));
    });
  });
}

// Subscribers first, each through subscriber_delete so their bindings go too, and the
// realm last: a delete that fails part way leaves a realm that can be deleted again.
void RedisDatastore::realm_delete(plugins::Executor on, std::string realm_name, plugins::StatusHandler handler) {
  _async_smembers(_subscriber_index_key(realm_name), [this, on, handler, realm_name](RedisError error, std::vector<std::string> users) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));

    auto failed = std::make_shared<std::string>();

    run_sequence(
        std::move(users),
        [this, on, realm_name, failed](std::string user, std::function<void()> next) {
          if (!failed->empty()) return next();

          auto identity = std::make_shared<types::SIPIdentity>("sip:" + user + "@" + realm_name);

          subscriber_delete(on, std::move(identity), [failed, next](plugins::Status status) mutable {
            // Already gone is not a failure: a concurrent delete may have taken the record.
            if (!status.ok && status.error != "subscriber_delete failed") *failed = status.error;
            next();
          });
        },
        [this, on, handler, realm_name, failed]() {
          if (!failed->empty()) return _complete(on, handler, plugins::Status::failure("realm_delete: " + *failed));
          _realm_delete_record(on, handler, realm_name);
        });
  });
}

void RedisDatastore::_realm_delete_record(plugins::Executor on, plugins::StatusHandler handler, std::string realm_name) {
  const auto index = _realm_index_key();

  _async_del(_realm_key(realm_name), [this, on, handler, index, realm_name](RedisError error, std::int64_t removed) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));

    _async_srem(index, realm_name, [this, on, handler, removed](RedisError error, bool) mutable {
      if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
      _complete(on, handler, _status(removed > 0, "realm_delete"));
    });
  });
}

void RedisDatastore::realm_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<types::Realm>>> handler) {
  using Answer = plugins::Result<std::vector<std::shared_ptr<types::Realm>>>;

  _read_index(
      _realm_index_key(), [](const std::string& name) { return _realm_key(name); },
      [this, on, handler](RedisError error, std::vector<std::pair<std::string, std::string>> records) mutable {
        if (error) return _complete(on, handler, Answer::failure(error.message()));

        std::vector<std::shared_ptr<types::Realm>> realms;
        for (const auto& [name, value] : records) {
          try {
            realms.push_back(_parse_realm(value));
          } catch (const std::exception& ex) {
            _logger->error("realm_list: skipping " + name + ": " + std::string(ex.what()));
          }
        }
        _complete(on, handler, Answer::success(std::move(realms)));
      });
}

void RedisDatastore::user_get(plugins::Executor on, std::string username, plugins::Handler<std::shared_ptr<types::User>> handler) {
  using Answer = plugins::Result<std::shared_ptr<types::User>>;

  _async_get(_user_key(username), [this, on, handler](RedisError error, std::optional<std::string> value) mutable {
    if (error) return _complete(on, handler, Answer::failure(error.message()));

    // Not found is a success carrying nothing.
    if (!value) return _complete(on, handler, Answer::success(nullptr));

    try {
      _complete(on, handler, Answer::success(_parse_user(*value)));
    } catch (const std::exception& ex) {
      _logger->error("user_get: " + std::string(ex.what()));
      _complete(on, handler, Answer::failure(ex.what()));
    }
  });
}

void RedisDatastore::user_create(plugins::Executor on, std::shared_ptr<types::User> user, plugins::StatusHandler handler) {
  if (!user || user->username.empty()) return _complete(on, handler, plugins::Status::failure("user_create: no user"));

  const auto key = _user_key(user->username);
  const auto index = _user_index_key();
  const auto name = user->key();
  const auto body = _serialise_user(user);

  // An existing user is a conflict. The key is case-folded, so "Tom" and "tom" collide.
  _async_exists(key, [this, on, handler, key, index, name, body](RedisError error, bool exists) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
    if (exists) return _complete(on, handler, plugins::Status::failure("user_create: " + name + " already exists"));

    _async_set(key, body, [this, on, handler, index, name](RedisError error, bool ok) mutable {
      if (error || !ok) return _complete(on, handler, plugins::Status::failure(error ? error.message() : "user_create: SET failed"));

      _async_sadd(index, name, [this, on, handler](RedisError error, bool) mutable {
        _complete(on, handler, error ? plugins::Status::failure(error.message()) : plugins::Status::success());
      });
    });
  });
}

void RedisDatastore::user_update(plugins::Executor on, std::shared_ptr<types::User> user, plugins::StatusHandler handler) {
  if (!user || user->username.empty()) return _complete(on, handler, plugins::Status::failure("user_update: no user"));

  const auto key = _user_key(user->username);
  const auto name = user->key();
  const auto body = _serialise_user(user);

  _async_exists(key, [this, on, handler, key, name, body](RedisError error, bool exists) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
    if (!exists) return _complete(on, handler, plugins::Status::failure("user_update: " + name + " does not exist"));

    _async_set(key, body, [this, on, handler](RedisError error, bool ok) mutable {
      _complete(on, handler, !error && ok ? plugins::Status::success() : plugins::Status::failure(error ? error.message() : "user_update: SET failed"));
    });
  });
}

void RedisDatastore::user_delete(plugins::Executor on, std::string username, plugins::StatusHandler handler) {
  const auto name = types::User::normalise(username);
  const auto index = _user_index_key();

  _async_del(_user_key(username), [this, on, handler, index, name](RedisError error, std::int64_t removed) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));

    _async_srem(index, name, [this, on, handler, name, removed](RedisError error, bool) mutable {
      if (error) return _complete(on, handler, plugins::Status::failure(error.message()));

      // The user's sessions go too, or they could not be revoked by username. The answer
      // still reflects whether there was a user to delete.
      _session_delete_all_for(name, [this, on, handler, removed](RedisError error) mutable {
        if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
        _complete(on, handler, _status(removed > 0, "user_delete"));
      });
    });
  });
}

void RedisDatastore::user_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<types::User>>> handler) {
  using Answer = plugins::Result<std::vector<std::shared_ptr<types::User>>>;

  _read_index(
      _user_index_key(), [](const std::string& name) { return _user_key(name); },
      [this, on, handler](RedisError error, std::vector<std::pair<std::string, std::string>> records) mutable {
        if (error) return _complete(on, handler, Answer::failure(error.message()));

        std::vector<std::shared_ptr<types::User>> users;
        for (const auto& [name, value] : records) {
          try {
            users.push_back(_parse_user(value));
          } catch (const std::exception& ex) {
            _logger->error("user_list: skipping " + name + ": " + std::string(ex.what()));
          }
        }
        _complete(on, handler, Answer::success(std::move(users)));
      });
}

// One script, so taking and renewing are atomic: two nodes asking at once cannot both win.
void RedisDatastore::lease(plugins::Executor on, std::string name, std::string holder, std::uint32_t seconds, plugins::Handler<bool> handler) {
  static constexpr const char* kTake = R"lua(
    local held = redis.call('GET', KEYS[1])
    if not held then redis.call('SET', KEYS[1], ARGV[1], 'EX', ARGV[2]) return 1 end
    if held == ARGV[1] then redis.call('EXPIRE', KEYS[1], ARGV[2]) return 1 end
    return 0
  )lua";

  boost::redis::request request;
  request.push("EVAL", kTake, 1, "athena:lease:" + name, holder, seconds);

  _async_integer("EVAL", std::move(request), [this, on, handler](RedisError error, std::int64_t taken) mutable {
    if (error) return _complete(on, handler, plugins::Result<bool>::failure(error.message()));
    _complete(on, handler, plugins::Result<bool>::success(taken == 1));
  });
}

// A trunk is one JSON value under its key, with an index of names, as a user is.
void RedisDatastore::trunk_get(plugins::Executor on, std::string name, plugins::Handler<std::shared_ptr<types::Trunk>> handler) {
  using Answer = plugins::Result<std::shared_ptr<types::Trunk>>;

  _async_get(_trunk_key(name), [this, on, handler](RedisError error, std::optional<std::string> value) mutable {
    if (error) return _complete(on, handler, Answer::failure(error.message()));
    if (!value) return _complete(on, handler, Answer::success(nullptr));

    try {
      _complete(on, handler, Answer::success(std::make_shared<types::Trunk>(types::Trunk::from_json(boost::json::parse(*value).as_object()))));
    } catch (const std::exception& ex) {
      _logger->error("trunk_get: " + std::string(ex.what()));
      _complete(on, handler, Answer::failure(ex.what()));
    }
  });
}

void RedisDatastore::trunk_create(plugins::Executor on, std::shared_ptr<types::Trunk> trunk, plugins::StatusHandler handler) {
  if (!trunk || trunk->name.empty()) return _complete(on, handler, plugins::Status::failure("trunk_create: no trunk"));

  const auto key = _trunk_key(trunk->name);
  const auto name = trunk->key();
  const auto body = boost::json::serialize(trunk->to_json());

  _async_exists(key, [this, on, handler, key, name, body](RedisError error, bool exists) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
    if (exists) return _complete(on, handler, plugins::Status::failure("trunk_create: " + name + " already exists"));

    _async_set(key, body, [this, on, handler, name](RedisError error, bool ok) mutable {
      if (error || !ok) return _complete(on, handler, plugins::Status::failure(error ? error.message() : "trunk_create: SET failed"));

      _async_sadd(_trunk_index_key(), name, [this, on, handler](RedisError error, bool) mutable {
        _complete(on, handler, error ? plugins::Status::failure(error.message()) : plugins::Status::success());
      });
    });
  });
}

void RedisDatastore::trunk_update(plugins::Executor on, std::shared_ptr<types::Trunk> trunk, plugins::StatusHandler handler) {
  if (!trunk || trunk->name.empty()) return _complete(on, handler, plugins::Status::failure("trunk_update: no trunk"));

  const auto key = _trunk_key(trunk->name);
  const auto name = trunk->key();
  const auto body = boost::json::serialize(trunk->to_json());

  _async_exists(key, [this, on, handler, key, name, body](RedisError error, bool exists) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
    if (!exists) return _complete(on, handler, plugins::Status::failure("trunk_update: " + name + " does not exist"));

    _async_set(key, body, [this, on, handler](RedisError error, bool ok) mutable {
      _complete(on, handler, !error && ok ? plugins::Status::success() : plugins::Status::failure(error ? error.message() : "trunk_update: SET failed"));
    });
  });
}

void RedisDatastore::trunk_delete(plugins::Executor on, std::string name, plugins::StatusHandler handler) {
  const auto normalised = types::Trunk::normalise(name);

  _async_del(_trunk_key(name), [this, on, handler, normalised](RedisError error, std::int64_t removed) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));

    _async_srem(_trunk_index_key(), normalised, [this, on, handler, removed](RedisError error, bool) mutable {
      if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
      _complete(on, handler, _status(removed > 0, "trunk_delete"));
    });
  });
}

void RedisDatastore::trunk_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<types::Trunk>>> handler) {
  using Answer = plugins::Result<std::vector<std::shared_ptr<types::Trunk>>>;

  _read_index(
      _trunk_index_key(), [](const std::string& name) { return _trunk_key(name); },
      [this, on, handler](RedisError error, std::vector<std::pair<std::string, std::string>> records) mutable {
        if (error) return _complete(on, handler, Answer::failure(error.message()));

        std::vector<std::shared_ptr<types::Trunk>> trunks;
        for (const auto& [name, value] : records) {
          try {
            trunks.push_back(std::make_shared<types::Trunk>(types::Trunk::from_json(boost::json::parse(value).as_object())));
          } catch (const std::exception& ex) {
            _logger->error("trunk_list: skipping " + name + ": " + std::string(ex.what()));
          }
        }
        _complete(on, handler, Answer::success(std::move(trunks)));
      });
}

// Creating over a held hash replaces the record, which is how last_seen_at moves: the
// contract has no session_update. Redis expires the record at its absolute expiry; idle
// expiry is the caller's rule (Session::has_expired).
void RedisDatastore::session_create(plugins::Executor on, types::Session session, plugins::StatusHandler handler) {
  if (session.token_hash.empty()) return _complete(on, handler, plugins::Status::failure("session_create: no token hash"));
  if (session.username.empty()) return _complete(on, handler, plugins::Status::failure("session_create: no user"));

  const auto now = std::time(nullptr);
  if (session.expires_at <= now) {
    _logger->warn("session_create: refusing to create already-expired session");
    return _complete(on, handler, plugins::Status::failure("session_create: already expired"));
  }

  // Filed under the user's case-folded key, so revoking by username finds it.
  session.username = types::User::normalise(session.username);

  const auto key = _session_key(session.token_hash);
  const auto index = _session_index_key(session.username);
  const auto hash = session.token_hash;
  const auto ttl = std::chrono::seconds(session.expires_at - now);
  const auto body = _serialise_session(session);

  _async_set_ex(key, body, ttl, [this, on, handler, index, hash](RedisError error, bool ok) mutable {
    if (error || !ok) return _complete(on, handler, plugins::Status::failure(error ? error.message() : "session_create: SETEX failed"));

    _async_sadd(index, hash, [this, on, handler](RedisError error, bool) mutable {
      _complete(on, handler, error ? plugins::Status::failure(error.message()) : plugins::Status::success());
    });
  });
}

void RedisDatastore::session_get(plugins::Executor on, std::string token_hash, plugins::Handler<std::shared_ptr<types::Session>> handler) {
  using Answer = plugins::Result<std::shared_ptr<types::Session>>;

  if (token_hash.empty()) return _complete(on, handler, Answer::success(nullptr));

  _async_get(_session_key(token_hash), [this, on, handler](RedisError error, std::optional<std::string> value) mutable {
    if (error) return _complete(on, handler, Answer::failure(error.message()));
    if (!value) return _complete(on, handler, Answer::success(nullptr));

    try {
      _complete(on, handler, Answer::success(_parse_session(*value)));
    } catch (const std::exception& ex) {
      _logger->error("session_get: " + std::string(ex.what()));
      _complete(on, handler, Answer::failure(ex.what()));
    }
  });
}

void RedisDatastore::session_delete(plugins::Executor on, std::string token_hash, plugins::StatusHandler handler) {
  // The record names its user, which is how the index entry is reached without KEYS.
  session_get(on, token_hash, [this, on, handler, token_hash](plugins::Result<std::shared_ptr<types::Session>> found) mutable {
    if (!found.ok) return _complete(on, handler, plugins::Status::failure(found.error));

    const auto username = found.value ? found.value->username : std::string();

    // Succeeds whether or not the key was there, as the contract requires; only Redis
    // refusing is a failure.
    _async_del(_session_key(token_hash), [this, on, handler, token_hash, username](RedisError error, std::int64_t) mutable {
      if (error) return _complete(on, handler, plugins::Status::failure(error.message()));

      if (username.empty()) return _complete(on, handler, plugins::Status::success());

      _async_srem(_session_index_key(username), token_hash, [this, on, handler](RedisError error, bool) mutable {
        _complete(on, handler, error ? plugins::Status::failure(error.message()) : plugins::Status::success());
      });
    });
  });
}

// Having nothing to revoke is success.
void RedisDatastore::session_delete_for_user(plugins::Executor on, std::string username, plugins::StatusHandler handler) {
  _session_delete_all_for(types::User::normalise(username), [this, on, handler](RedisError error) mutable {
    _complete(on, handler, error ? plugins::Status::failure(error.message()) : plugins::Status::success());
  });
}

void RedisDatastore::_session_delete_all_for(std::string key, std::function<void(RedisError)> done) {
  const auto index = _session_index_key(key);

  _async_smembers(index, [this, index, done](RedisError error, std::vector<std::string> hashes) mutable {
    if (error) return done(error);

    run_sequence(
        std::move(hashes),
        [this](std::string hash, std::function<void()> next) { _async_del(_session_key(hash), [next](RedisError, std::int64_t) mutable { next(); }); },
        [this, index, done]() {
          // The index is deleted whole: it holds nothing that outlives its sessions.
          _async_del(index, [done](RedisError error, std::int64_t) mutable { done(error); });
        });
  });
}

std::shared_ptr<types::Subscriber> RedisDatastore::_parse_subscriber(const std::string& value, std::shared_ptr<types::SIPIdentity> identity) {
  const auto parsed = boost::json::parse(value);
  const auto& obj = parsed.as_object();

  auto subscriber = std::make_shared<types::Subscriber>();
  subscriber->id = json_uint64(obj, "id");
  subscriber->identity = std::move(identity);
  subscriber->ha1 = json_string(obj, "ha1");

  // Optional: a subscriber imported as a bare MD5 hash has no SHA-256 credential.
  if (obj.if_contains("ha1_sha256")) subscriber->ha1_sha256 = json_string(obj, "ha1_sha256");
  if (obj.contains("media_profile")) subscriber->media_profile = types::MediaPolicy::parse_profiles(json_string(obj, "media_profile"));
  if (const auto* attributes = obj.if_contains("attributes"); attributes != nullptr && attributes->is_object()) {
    subscriber->attributes = attributes->as_object();
  }
  return subscriber;
}

void RedisDatastore::subscriber_get(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity,
                                    plugins::Handler<std::shared_ptr<types::Subscriber>> handler) {
  using Answer = plugins::Result<std::shared_ptr<types::Subscriber>>;

  if (!identity || !identity->uri) return _complete(on, handler, Answer::failure("subscriber_get: no identity"));

  _async_get(_subscriber_key(identity->uri->host, identity->uri->user),
             [this, on, handler, identity](RedisError error, std::optional<std::string> value) mutable {
               if (error) return _complete(on, handler, Answer::failure(error.message()));
               if (!value) return _complete(on, handler, Answer::success(nullptr));

               try {
                 _complete(on, handler, Answer::success(_parse_subscriber(*value, std::move(identity))));
               } catch (const std::exception& ex) {
                 _logger->error("subscriber_get: " + std::string(ex.what()));
                 _complete(on, handler, Answer::failure(ex.what()));
               }
             });
}

void RedisDatastore::subscriber_create(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, plugins::StatusHandler handler) {
  if (!subscriber || !subscriber->identity || !subscriber->identity->uri)
    return _complete(on, handler, plugins::Status::failure("subscriber_create: no subscriber"));

  const auto& uri = subscriber->identity->uri;
  const auto key = _subscriber_key(uri->host, uri->user);
  const auto index = _subscriber_index_key(uri->host);
  const auto user = uri->user;
  const auto body = _serialise_subscriber(subscriber);

  _async_exists(key, [this, on, handler, key, index, user, body](RedisError error, bool exists) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
    if (exists) return _complete(on, handler, plugins::Status::failure("subscriber_create: " + user + " already exists"));

    _async_set(key, body, [this, on, handler, index, user](RedisError error, bool ok) mutable {
      if (error || !ok) return _complete(on, handler, plugins::Status::failure(error ? error.message() : "subscriber_create: SET failed"));

      _async_sadd(index, user, [this, on, handler](RedisError error, bool) mutable {
        _complete(on, handler, error ? plugins::Status::failure(error.message()) : plugins::Status::success());
      });
    });
  });
}

void RedisDatastore::subscriber_update(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, plugins::StatusHandler handler) {
  if (!subscriber || !subscriber->identity || !subscriber->identity->uri)
    return _complete(on, handler, plugins::Status::failure("subscriber_update: no subscriber"));

  const auto& uri = subscriber->identity->uri;
  const auto key = _subscriber_key(uri->host, uri->user);
  const auto user = uri->user;
  const auto body = _serialise_subscriber(subscriber);

  _async_exists(key, [this, on, handler, key, user, body](RedisError error, bool exists) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
    if (!exists) return _complete(on, handler, plugins::Status::failure("subscriber_update: " + user + " does not exist"));

    _async_set(key, body, [this, on, handler](RedisError error, bool ok) mutable {
      _complete(on, handler, !error && ok ? plugins::Status::success() : plugins::Status::failure(error ? error.message() : "subscriber_update: SET failed"));
    });
  });
}

void RedisDatastore::subscriber_delete(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity, plugins::StatusHandler handler) {
  if (!identity || !identity->uri) return _complete(on, handler, plugins::Status::failure("subscriber_delete: no identity"));

  const auto realm_name = identity->uri->host;
  const auto user = identity->uri->user;

  // Read before deleting: the location index is keyed on the subscriber id.
  subscriber_get(on, identity, [this, on, handler, realm_name, user](plugins::Result<std::shared_ptr<types::Subscriber>> found) mutable {
    if (!found.ok) return _complete(on, handler, plugins::Status::failure(found.error));

    auto subscriber = found.value;

    _async_del(_subscriber_key(realm_name, user), [this, on, handler, realm_name, user, subscriber](RedisError error, std::int64_t removed) mutable {
      if (error) return _complete(on, handler, plugins::Status::failure(error.message()));

      _async_srem(_subscriber_index_key(realm_name), user, [this, on, handler, subscriber, removed](RedisError error, bool) mutable {
        if (error) return _complete(on, handler, plugins::Status::failure(error.message()));

        if (!subscriber) return _complete(on, handler, _status(removed > 0, "subscriber_delete"));

        // A deleted subscriber keeps no bindings.
        const auto index = _location_index_key(subscriber->id);

        _async_smembers(index, [this, on, handler, index, removed](RedisError error, std::vector<std::string> keys) mutable {
          if (error) return _complete(on, handler, plugins::Status::failure(error.message()));

          run_sequence(
              std::move(keys), [this](std::string key, std::function<void()> next) { _async_del(key, [next](RedisError, std::int64_t) mutable { next(); }); },
              [this, on, handler, index, removed]() {
                _async_del(index, [this, on, handler, removed](RedisError error, std::int64_t) mutable {
                  if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
                  _complete(on, handler, _status(removed > 0, "subscriber_delete"));
                });
              });
        });
      });
    });
  });
}

void RedisDatastore::subscriber_list(plugins::Executor on, std::string realm_name, plugins::Handler<std::vector<std::shared_ptr<types::Subscriber>>> handler) {
  using Answer = plugins::Result<std::vector<std::shared_ptr<types::Subscriber>>>;

  _read_index(
      _subscriber_index_key(realm_name), [realm_name](const std::string& user) { return _subscriber_key(realm_name, user); },
      [this, on, handler, realm_name](RedisError error, std::vector<std::pair<std::string, std::string>> records) mutable {
        if (error) return _complete(on, handler, Answer::failure(error.message()));

        std::vector<std::shared_ptr<types::Subscriber>> subscribers;
        for (const auto& [user, value] : records) {
          try {
            subscribers.push_back(_parse_subscriber(value, std::make_shared<types::SIPIdentity>("sip:" + user + "@" + realm_name)));
          } catch (const std::exception& ex) {
            _logger->error("subscriber_list: skipping " + user + ": " + std::string(ex.what()));
          }
        }
        _complete(on, handler, Answer::success(std::move(subscribers)));
      });
}

void RedisDatastore::subscriber_register(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, types::Location binding,
                                         std::uint32_t expires_seconds, plugins::StatusHandler handler) {
  if (!subscriber || !binding.contact) return _complete(on, handler, plugins::Status::failure("subscriber_register: no subscriber or contact"));

  const auto contact = binding.contact;
  const std::uint16_t port = contact->port.value_or(0);
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  const bool is_nat = Util::is_ipv4(contact->host) && Util::is_ipv4_private(contact->host);

  constexpr std::int64_t kDefaultRegistrationSeconds = 3600;

  // The lifetime the registrar granted, which Redis expires the binding on.
  const std::int64_t ttl = expires_seconds > 0 ? static_cast<std::int64_t>(expires_seconds) : kDefaultRegistrationSeconds;

  boost::json::object location;
  location["subscriber_id"] = subscriber->id;
  location["contact"] = contact->to_string();
  location["user"] = contact->user;
  location["host"] = contact->host;
  location["port"] = port;
  location["registered_at"] = now;
  location["expires_at"] = now + ttl;
  location["nat"] = is_nat ? "Y" : "N";
  if (!binding.path.empty()) location["path"] = binding.path;

  // What another node needs to use this binding. Written only when set; empty on a
  // single node.
  if (!binding.flow_id.empty()) location["flow_id"] = binding.flow_id;
  if (!binding.node_id.empty()) location["node_id"] = binding.node_id;
  if (!binding.instance.empty()) location["instance"] = binding.instance;
  if (binding.reg_id != 0) location["reg_id"] = binding.reg_id;
  if (binding.push) location["push"] = true;

  const auto key = _location_key(subscriber->id, contact->user, contact->host, port);
  const auto index = _location_index_key(subscriber->id);

  _async_set_ex(key, boost::json::serialize(location), std::chrono::seconds(ttl), [this, on, handler, key, index](RedisError error, bool ok) mutable {
    if (error || !ok) return _complete(on, handler, plugins::Status::failure(error ? error.message() : "subscriber_register: SETEX failed"));

    // location_list reads this index, so listing never needs KEYS.
    _async_sadd(index, key, [this, on, handler](RedisError error, bool) mutable {
      _complete(on, handler, error ? plugins::Status::failure(error.message()) : plugins::Status::success());
    });
  });
}

void RedisDatastore::subscriber_unregister(plugins::Executor on, std::shared_ptr<types::Subscriber> subscriber, std::shared_ptr<types::SIPUri> contact,
                                           plugins::StatusHandler handler) {
  if (!subscriber || !contact) return _complete(on, handler, plugins::Status::failure("subscriber_unregister: no subscriber or contact"));

  const std::uint16_t port = contact->port.value_or(0);
  const auto key = _location_key(subscriber->id, contact->user, contact->host, port);
  const auto index = _location_index_key(subscriber->id);

  _async_del(key, [this, on, handler, key, index](RedisError error, std::int64_t removed) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));

    _async_srem(index, key, [this, on, handler, removed](RedisError error, bool) mutable {
      if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
      _complete(on, handler, _status(removed > 0, "subscriber_unregister"));
    });
  });
}

void RedisDatastore::location_list(plugins::Executor on, std::uint64_t subscriber_id, plugins::Handler<std::vector<types::Location>> handler) {
  using Answer = plugins::Result<std::vector<types::Location>>;

  // The index holds the binding keys themselves; an expired binding leaves it here.
  _read_index(
      _location_index_key(subscriber_id), [](const std::string& key) { return key; },
      [this, on, handler](RedisError error, std::vector<std::pair<std::string, std::string>> records) mutable {
        if (error) return _complete(on, handler, Answer::failure(error.message()));

        std::vector<types::Location> locations;
        for (const auto& [key, value] : records) {
          try {
            locations.push_back(parse_location(value));
          } catch (const std::exception& ex) {
            // One unreadable binding must not hide the others (RFC 3261 16.5).
            _logger->error("location_list: skipping " + key + ": " + std::string(ex.what()));
          }
        }
        _complete(on, handler, Answer::success(std::move(locations)));
      });
}

void RedisDatastore::nonce_create(plugins::Executor on, std::string nonce, std::time_t expires_at, plugins::StatusHandler handler) {
  const auto now = std::time(nullptr);
  if (expires_at <= now) {
    _logger->warn("nonce_create: refusing to create already-expired nonce");
    return _complete(on, handler, plugins::Status::failure("nonce_create: already expired"));
  }

  _async_set_ex(_nonce_key(nonce), "1", std::chrono::seconds(expires_at - now), [this, on, handler](RedisError error, bool ok) mutable {
    _complete(on, handler, !error && ok ? plugins::Status::success() : plugins::Status::failure(error ? error.message() : "nonce_create: SETEX failed"));
  });
}

void RedisDatastore::nonce_check(plugins::Executor on, std::string nonce, plugins::Handler<bool> handler) {
  _async_exists(_nonce_key(nonce), [this, on, handler](RedisError error, bool exists) mutable {
    _complete(on, handler, error ? plugins::Result<bool>::failure(error.message()) : plugins::Result<bool>::success(exists));
  });
}

void RedisDatastore::call_create(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) {
  if (!call || call->id.empty()) return _complete(on, handler, plugins::Status::failure("call_create: no call"));

  const auto id = call->id;
  const auto index = _call_index_key();

  _async_set(_call_key(id), _serialise_call(call), [this, on, handler, id, index](RedisError error, bool ok) mutable {
    if (error || !ok) return _complete(on, handler, plugins::Status::failure(error ? error.message() : "call_create: SET failed"));

    _async_sadd(index, id, [this, on, handler](RedisError error, bool) mutable {
      _complete(on, handler, error ? plugins::Status::failure(error.message()) : plugins::Status::success());
    });
  });
}

void RedisDatastore::call_update(plugins::Executor on, std::shared_ptr<Call> call, plugins::StatusHandler handler) {
  if (!call || call->id.empty()) return _complete(on, handler, plugins::Status::failure("call_update: no call"));

  const auto key = _call_key(call->id);
  const auto id = call->id;
  const auto body = _serialise_call(call);
  const bool ended = call->state == Call::State::Closed;

  _async_exists(key, [this, on, handler, key, id, body, ended](RedisError error, bool exists) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
    if (!exists) return _complete(on, handler, plugins::Status::failure("call_update: " + id + " does not exist"));

    auto written = [this, on, handler](RedisError error, bool ok) mutable {
      _complete(on, handler, !error && ok ? plugins::Status::success() : plugins::Status::failure(error ? error.message() : "call_update: SET failed"));
    };

    // An ended call is a record kept for the retention period. call_list removes its id
    // from the index once it has expired.
    if (ended && _call_retention != 0) return _async_set_ex(key, body, std::chrono::seconds(_call_retention), std::move(written));

    _async_set(key, body, std::move(written));
  });
}

bool RedisDatastore::configure(const YAML::Node& own_root, const Config& system) {
  (void)own_root;
  _call_retention = system.calls_history_retention;
  return true;
}

void RedisDatastore::call_get(plugins::Executor on, std::string id, plugins::Handler<std::shared_ptr<Call>> handler) {
  using Answer = plugins::Result<std::shared_ptr<Call>>;

  _async_get(_call_key(id), [this, on, handler](RedisError error, std::optional<std::string> value) mutable {
    if (error) return _complete(on, handler, Answer::failure(error.message()));
    if (!value) return _complete(on, handler, Answer::success(nullptr));

    try {
      _complete(on, handler, Answer::success(_parse_call(*value)));
    } catch (const std::exception& ex) {
      _logger->error("call_get: " + std::string(ex.what()));
      _complete(on, handler, Answer::failure(ex.what()));
    }
  });
}

void RedisDatastore::call_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<Call>>> handler) {
  using Answer = plugins::Result<std::vector<std::shared_ptr<Call>>>;

  _read_index(
      _call_index_key(), [](const std::string& id) { return _call_key(id); },
      [this, on, handler](RedisError error, std::vector<std::pair<std::string, std::string>> records) mutable {
        if (error) return _complete(on, handler, Answer::failure(error.message()));

        std::vector<std::shared_ptr<Call>> calls;
        for (const auto& [id, value] : records) {
          try {
            calls.push_back(_parse_call(value));
          } catch (const std::exception& ex) {
            _logger->error("call_list: skipping " + id + ": " + std::string(ex.what()));
          }
        }
        _complete(on, handler, Answer::success(std::move(calls)));
      });
}

void RedisDatastore::_async_sadd(std::string key, std::string member, BoolCallback callback) {
  boost::redis::request request;
  request.push("SADD", key, member);

  _async_integer("SADD", std::move(request), [callback = std::move(callback)](RedisError error, std::int64_t value) mutable { callback(error, value >= 0); });
}

void RedisDatastore::_async_srem(std::string key, std::string member, BoolCallback callback) {
  boost::redis::request request;
  request.push("SREM", key, member);

  _async_integer("SREM", std::move(request), [callback = std::move(callback)](RedisError error, std::int64_t value) mutable { callback(error, value > 0); });
}

void RedisDatastore::_async_smembers(std::string key, StringsCallback callback) {
  boost::redis::request request;
  request.push("SMEMBERS", key);

  _async_strings("SMEMBERS", std::move(request), std::move(callback));
}

std::string RedisDatastore::_serialise_realm(const std::shared_ptr<types::Realm>& realm) {
  boost::json::object obj;
  obj["id"] = realm->id;
  obj["name"] = realm->name;
  obj["nonce_secret"] = realm->nonce_secret;
  obj["nonce_expiry"] = realm->nonce_expiry;
  obj["registration_timeout"] = realm->registration_timeout;
  obj["registration_minimum"] = realm->registration_minimum;
  // Only what the realm chose is stored; an unset setting inherits the server's default.
  // The profile is stored under the key "media_profiles".
  if (realm->behaviour.media_anchor) obj["media_anchor"] = *realm->behaviour.media_anchor;
  if (realm->behaviour.media_profile) obj["media_profiles"] = types::MediaPolicy::to_string(*realm->behaviour.media_profile);
  if (realm->behaviour.qualify_interval) obj["qualify_interval"] = *realm->behaviour.qualify_interval;
  if (realm->behaviour.rewrite_contact) obj["rewrite_contact"] = *realm->behaviour.rewrite_contact;
  if (!realm->attributes.empty()) obj["attributes"] = realm->attributes;
  return boost::json::serialize(obj);
}

std::shared_ptr<types::Realm> RedisDatastore::_parse_realm(const std::string& value) const {
  const auto parsed = boost::json::parse(value);
  const auto& obj = parsed.as_object();

  auto realm = std::make_shared<types::Realm>(json_string(obj, "name"));
  realm->id = json_uint64(obj, "id");
  realm->nonce_secret = json_string(obj, "nonce_secret");
  realm->nonce_expiry = json_uint32(obj, "nonce_expiry");
  realm->registration_timeout = json_uint32(obj, "registration_timeout");
  realm->registration_minimum = json_uint32(obj, "registration_minimum");

  // Absent is unset. A stored value that does not parse leaves the setting unset.
  if (const auto* anchor = obj.if_contains("media_anchor"); anchor != nullptr && anchor->is_bool()) realm->behaviour.media_anchor = anchor->as_bool();
  if (obj.contains("media_profiles")) realm->behaviour.media_profile = types::MediaPolicy::parse_profiles(json_string(obj, "media_profiles"));
  if (const auto* rewrite = obj.if_contains("rewrite_contact"); rewrite != nullptr && rewrite->is_bool()) realm->behaviour.rewrite_contact = rewrite->as_bool();
  if (const auto* qualify = obj.if_contains("qualify_interval"); qualify != nullptr && qualify->is_number()) {
    const auto seconds = qualify->to_number<std::int64_t>();
    if (types::Behaviour::valid_qualify_interval(seconds)) realm->behaviour.qualify_interval = static_cast<std::uint32_t>(seconds);
  }
  if (const auto* attributes = obj.if_contains("attributes"); attributes != nullptr && attributes->is_object()) realm->attributes = attributes->as_object();

  return realm;
}

std::string RedisDatastore::_serialise_user(const std::shared_ptr<types::User>& user) {
  boost::json::object obj;

  // The username as given, not the case-folded key.
  obj["username"] = user->username;
  obj["display_name"] = user->display_name;
  obj["password_hash"] = user->password_hash;
  obj["disabled"] = user->disabled;
  obj["created_at"] = static_cast<std::uint64_t>(user->created_at);
  obj["last_login_at"] = static_cast<std::uint64_t>(user->last_login_at);

  boost::json::array roles;
  for (const auto& role : user->roles) roles.push_back(boost::json::string(role));
  obj["roles"] = std::move(roles);

  return boost::json::serialize(obj);
}

std::shared_ptr<types::User> RedisDatastore::_parse_user(const std::string& value) const {
  const auto parsed = boost::json::parse(value);
  const auto& obj = parsed.as_object();

  auto user = std::make_shared<types::User>();
  user->username = json_string(obj, "username");
  user->display_name = json_string(obj, "display_name");
  user->password_hash = json_string(obj, "password_hash");
  user->created_at = static_cast<std::time_t>(json_uint64(obj, "created_at"));
  user->last_login_at = static_cast<std::time_t>(json_uint64(obj, "last_login_at"));

  if (const auto* disabled = obj.if_contains("disabled"); disabled != nullptr && disabled->is_bool()) user->disabled = disabled->as_bool();

  // An unknown role is kept rather than dropped, so an older node rewriting a user does
  // not strip a role a newer node granted. It grants nothing; the API validates roles on
  // the way in.
  if (const auto* roles = obj.if_contains("roles"); roles != nullptr && roles->is_array()) {
    for (const auto& role : roles->as_array()) {
      if (role.is_string()) user->roles.push_back(std::string(role.as_string().c_str()));
    }
  }

  return user;
}

std::string RedisDatastore::_serialise_session(const types::Session& session) {
  boost::json::object obj;
  obj["token_hash"] = session.token_hash;
  obj["username"] = session.username;
  obj["created_at"] = static_cast<std::uint64_t>(session.created_at);
  obj["expires_at"] = static_cast<std::uint64_t>(session.expires_at);
  obj["last_seen_at"] = static_cast<std::uint64_t>(session.last_seen_at);
  return boost::json::serialize(obj);
}

std::shared_ptr<types::Session> RedisDatastore::_parse_session(const std::string& value) const {
  const auto parsed = boost::json::parse(value);
  const auto& obj = parsed.as_object();

  auto session = std::make_shared<types::Session>();
  session->token_hash = json_string(obj, "token_hash");
  session->username = json_string(obj, "username");
  session->created_at = static_cast<std::time_t>(json_uint64(obj, "created_at"));
  session->expires_at = static_cast<std::time_t>(json_uint64(obj, "expires_at"));
  session->last_seen_at = static_cast<std::time_t>(json_uint64(obj, "last_seen_at"));

  return session;
}

std::string RedisDatastore::_serialise_subscriber(const std::shared_ptr<types::Subscriber>& subscriber) {
  boost::json::object obj;
  obj["id"] = subscriber->id;
  obj["ha1"] = subscriber->ha1;

  // Written only when present: an MD5-only subscriber has no SHA-256 credential.
  if (!subscriber->ha1_sha256.empty()) obj["ha1_sha256"] = subscriber->ha1_sha256;

  // Written only when the subscriber chose one; absent takes the realm's.
  if (subscriber->media_profile) obj["media_profile"] = types::MediaPolicy::to_string(*subscriber->media_profile);
  if (!subscriber->attributes.empty()) obj["attributes"] = subscriber->attributes;
  obj["uri"] = subscriber->identity->uri->to_string();
  return boost::json::serialize(obj);
}

std::string RedisDatastore::_serialise_call(const std::shared_ptr<Call>& call) {
  boost::json::object obj;
  obj["id"] = call->id;
  obj["state"] = Call::state_to_string(call->state);
  obj["created_at"] = static_cast<std::uint64_t>(call->created_at);
  obj["answered_at"] = static_cast<std::uint64_t>(call->answered_at);
  obj["ended_at"] = static_cast<std::uint64_t>(call->ended_at);
  obj["focus"] = call->focus ? call->focus->to_string() : "";
  obj["node"] = call->node;
  obj["media_engine"] = call->media_engine;

  // Participants are persisted; their media streams are not, since a relay set belongs to
  // the node that allocated it.
  boost::json::array participants;
  for (const auto& participant : call->participants) {
    boost::json::object entry;
    entry["identity"] = participant.identity ? participant.identity->to_string() : "";
    entry["node_id"] = participant.node_id;
    // The dialog's identity only. The route set and sequence numbers belong to the node on
    // the path.
    entry["call_id"] = participant.dialog ? participant.dialog->call_id : "";
    entry["caller_tag"] = participant.dialog ? participant.dialog->caller_tag : "";
    entry["callee_tag"] = participant.dialog ? participant.dialog->callee_tag : "";
    entry["originator"] = participant.originator;
    if (!participant.trunk.empty()) entry["trunk"] = participant.trunk;
    participants.push_back(std::move(entry));
  }

  obj["participants"] = std::move(participants);
  return boost::json::serialize(obj);
}

std::shared_ptr<Call> RedisDatastore::_parse_call(const std::string& value) const {
  const auto parsed = boost::json::parse(value);
  const auto& obj = parsed.as_object();

  auto call = std::make_shared<Call>();
  call->id = json_string(obj, "id");
  call->created_at = static_cast<std::time_t>(json_uint64(obj, "created_at"));
  call->answered_at = static_cast<std::time_t>(json_uint64(obj, "answered_at"));
  call->ended_at = static_cast<std::time_t>(json_uint64(obj, "ended_at"));

  const auto state = json_string(obj, "state");
  for (const auto candidate :
       {Call::State::Initial, Call::State::Trying, Call::State::Ringing, Call::State::Connected, Call::State::Closing, Call::State::Closed}) {
    if (Call::state_to_string(candidate) == state) {
      call->state = candidate;
      break;
    }
  }

  const auto focus = json_string(obj, "focus");
  if (!focus.empty()) call->focus = std::make_shared<types::SIPUri>(focus);

  if (obj.contains("node")) call->node = json_string(obj, "node");
  if (obj.contains("media_engine")) call->media_engine = json_string(obj, "media_engine");

  if (const auto* participants = obj.if_contains("participants"); participants != nullptr && participants->is_array()) {
    for (const auto& entry : participants->as_array()) {
      const auto& participant_obj = entry.as_object();

      auto& participant = call->add_participant(std::make_shared<types::SIPIdentity>(json_string(participant_obj, "identity")), nullptr,
                                                participant_obj.at("originator").as_bool());
      participant.node_id = json_string(participant_obj, "node_id");
      // Absent from a record written before trunks, and from any leg that ran over none.
      if (const auto* trunk = participant_obj.if_contains("trunk"); trunk != nullptr && trunk->is_string()) participant.trunk = trunk->as_string().c_str();

      const auto call_id = json_string(participant_obj, "call_id");

      if (!call_id.empty()) {
        participant.dialog = std::make_shared<types::Dialog>();
        participant.dialog->call_id = call_id;
        participant.dialog->caller_tag = json_string(participant_obj, "caller_tag");
        participant.dialog->callee_tag = json_string(participant_obj, "callee_tag");
      }
    }
  }

  return call;
}

void RedisDatastore::_apply_url(std::shared_ptr<types::URL> url) {
  _host = "127.0.0.1";
  _port = 6379;
  _username = "default";
  _password.clear();
  _database_index = 0;
  _use_ssl = false;

  if (!url) {
    return;
  }

  std::string value = url->to_string();

  const auto scheme_pos = value.find("://");
  std::string scheme;
  if (scheme_pos != std::string::npos) {
    scheme = value.substr(0, scheme_pos);
    value.erase(0, scheme_pos + 3);
  } else {
    scheme = url->scheme;
  }

  _use_ssl = scheme == "rediss" || scheme == "redis+ssl";

  const auto path_pos = value.find('/');
  std::string authority = path_pos == std::string::npos ? value : value.substr(0, path_pos);
  std::string path = path_pos == std::string::npos ? std::string{} : value.substr(path_pos + 1);

  const auto query_pos = path.find('?');
  if (query_pos != std::string::npos) {
    path.erase(query_pos);
  }

  if (!path.empty()) {
    _database_index = static_cast<std::int32_t>(std::stol(path));
  }

  const auto auth_pos = authority.rfind('@');
  if (auth_pos != std::string::npos) {
    const auto auth = authority.substr(0, auth_pos);
    authority.erase(0, auth_pos + 1);

    const auto colon_pos = auth.find(':');
    if (colon_pos == std::string::npos) {
      _password = auth;
    } else {
      _username = auth.substr(0, colon_pos);
      _password = auth.substr(colon_pos + 1);
      if (_username.empty()) {
        _username = "default";
      }
    }
  }

  const auto port_pos = authority.rfind(':');
  if (port_pos == std::string::npos) {
    if (!authority.empty()) {
      _host = authority;
    }
    return;
  }

  _host = authority.substr(0, port_pos);
  _port = parse_port(authority.substr(port_pos + 1));

  if (_host.empty()) {
    _host = "127.0.0.1";
  }
}

boost::redis::config RedisDatastore::_make_config() const {
  boost::redis::config cfg;
  cfg.addr.host = _host;
  cfg.addr.port = std::to_string(_port);
  cfg.username = _username;
  cfg.password = _password;
  cfg.database_index = _database_index;
  cfg.use_ssl = _use_ssl;
  return cfg;
}

void RedisDatastore::_async_ping(BoolCallback callback) {
  boost::redis::request req;
  req.push("PING");
  _async_ok("PING", std::move(req), std::move(callback));
}

void RedisDatastore::_async_set(std::string key, std::string value, BoolCallback callback) {
  boost::redis::request req;
  req.push("SET", key, value);
  _async_ok("SET", std::move(req), std::move(callback));
}

void RedisDatastore::_async_set_ex(std::string key, std::string value, std::chrono::seconds expiry, BoolCallback callback) {
  boost::redis::request req;
  req.push("SET", key, value, "EX", expiry.count());
  _async_ok("SET EX", std::move(req), std::move(callback));
}

void RedisDatastore::_async_get(std::string key, StringCallback callback) {
  boost::redis::request req;
  req.push("GET", key);
  _async_string("GET", std::move(req), std::move(callback));
}

void RedisDatastore::_async_del(std::string key, IntegerCallback callback) {
  boost::redis::request req;
  req.push("DEL", key);
  _async_integer("DEL", std::move(req), std::move(callback));
}

void RedisDatastore::_async_exists(std::string key, BoolCallback callback) {
  boost::redis::request req;
  req.push("EXISTS", key);

  _async_integer("EXISTS", std::move(req),
                 [callback = std::move(callback)](RedisError error, std::int64_t value) mutable { callback(std::move(error), value > 0); });
}

void RedisDatastore::_async_ok(std::string operation, boost::redis::request request, BoolCallback callback) {
  if (!_connection) {
    callback(_not_connected_error(), false);
    return;
  }

  auto start = std::chrono::high_resolution_clock::now();
  auto logger = _logger;
  auto conn = _connection;
  auto req = std::make_shared<boost::redis::request>(std::move(request));
  auto resp = std::make_shared<boost::redis::response<std::string>>();

  // boost::redis::connection is not thread safe: every operation starts on its executor.
  boost::asio::post(_io_context, [conn, req, resp, logger, operation = std::move(operation), callback = std::move(callback), start]() mutable {
    conn->async_exec(
        *req, *resp,
        [logger, operation = std::move(operation), req, resp, callback = std::move(callback), start](boost::system::error_code ec, std::size_t) mutable {
          if (ec) {
            logger->error("Error during: " + operation + ": " + ec.message());
            callback(RedisError(ec), false);
            return;
          }

          auto& result = std::get<0>(*resp);
          if (!result.has_value()) {
            auto error = make_redis_response_error(result.error().diagnostic);
            logger->error("Error during: " + operation + ": " + error.message());
            callback(std::move(error), false);
            return;
          }

          const auto& value = result.value();
          const bool ok = value == "OK" || value == "PONG";

          std::ostringstream oss;
          oss << std::fixed << std::setprecision(3)
              << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
          logger->debug(operation + " (" + oss.str() + "ms)");

          callback(RedisError{}, ok);
        });
  });
}

void RedisDatastore::_async_string(std::string operation, boost::redis::request request, StringCallback callback) {
  if (!_connection) {
    callback(_not_connected_error(), std::nullopt);
    return;
  }

  auto start = std::chrono::high_resolution_clock::now();
  auto logger = _logger;
  auto conn = _connection;
  auto req = std::make_shared<boost::redis::request>(std::move(request));
  auto resp = std::make_shared<boost::redis::response<std::optional<std::string>>>();

  // boost::redis::connection is not thread safe: every operation starts on its executor.
  boost::asio::post(_io_context, [conn, req, resp, logger, operation = std::move(operation), callback = std::move(callback), start]() mutable {
    conn->async_exec(
        *req, *resp,
        [logger, operation = std::move(operation), req, resp, callback = std::move(callback), start](boost::system::error_code ec, std::size_t) mutable {
          if (ec) {
            logger->error("Error during: " + operation + ": " + ec.message());
            callback(RedisError(ec), std::nullopt);
            return;
          }

          auto& result = std::get<0>(*resp);
          if (!result.has_value()) {
            auto error = make_redis_response_error(result.error().diagnostic);
            logger->error("Error during: " + operation + ": " + error.message());
            callback(std::move(error), std::nullopt);
            return;
          }

          std::ostringstream oss;
          oss << std::fixed << std::setprecision(3)
              << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
          logger->debug(operation + " (" + oss.str() + "ms)");

          callback(RedisError{}, result.value());
        });
  });
}

void RedisDatastore::_async_integer(std::string operation, boost::redis::request request, IntegerCallback callback) {
  if (!_connection) {
    callback(_not_connected_error(), 0);
    return;
  }

  auto start = std::chrono::high_resolution_clock::now();
  auto logger = _logger;
  auto conn = _connection;
  auto req = std::make_shared<boost::redis::request>(std::move(request));
  auto resp = std::make_shared<boost::redis::response<long long>>();

  // boost::redis::connection is not thread safe: every operation starts on its executor.
  boost::asio::post(_io_context, [conn, req, resp, logger, operation = std::move(operation), callback = std::move(callback), start]() mutable {
    conn->async_exec(
        *req, *resp,
        [logger, operation = std::move(operation), req, resp, callback = std::move(callback), start](boost::system::error_code ec, std::size_t) mutable {
          if (ec) {
            logger->error("Error during: " + operation + ": " + ec.message());
            callback(RedisError(ec), 0);
            return;
          }

          auto& result = std::get<0>(*resp);
          if (!result.has_value()) {
            auto error = make_redis_response_error(result.error().diagnostic);
            logger->error("Error during: " + operation + ": " + error.message());
            callback(std::move(error), 0);
            return;
          }

          std::ostringstream oss;
          oss << std::fixed << std::setprecision(3)
              << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
          logger->debug(operation + " (" + oss.str() + "ms)");

          callback(RedisError{}, static_cast<std::int64_t>(result.value()));
        });
  });
}

void RedisDatastore::_async_strings(std::string operation, boost::redis::request request, StringsCallback callback) {
  if (!_connection) {
    callback(_not_connected_error(), {});
    return;
  }

  auto start = std::chrono::high_resolution_clock::now();
  auto logger = _logger;
  auto conn = _connection;
  auto req = std::make_shared<boost::redis::request>(std::move(request));
  auto resp = std::make_shared<boost::redis::response<std::vector<std::string>>>();

  // boost::redis::connection is not thread safe: every operation starts on its executor.
  boost::asio::post(_io_context, [conn, req, resp, logger, operation = std::move(operation), callback = std::move(callback), start]() mutable {
    conn->async_exec(
        *req, *resp,
        [logger, operation = std::move(operation), req, resp, callback = std::move(callback), start](boost::system::error_code ec, std::size_t) mutable {
          if (ec) {
            logger->error("Error during: " + operation + ": " + ec.message());
            callback(RedisError(ec), {});
            return;
          }

          auto& result = std::get<0>(*resp);
          if (!result.has_value()) {
            auto error = make_redis_response_error(result.error().diagnostic);
            logger->error("Error during: " + operation + ": " + error.message());
            callback(std::move(error), {});
            return;
          }

          std::ostringstream oss;
          oss << std::fixed << std::setprecision(3)
              << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
          logger->debug(operation + " (" + oss.str() + "ms)");

          callback(RedisError{}, result.value());
        });
  });
}

void RedisDatastore::_async_mget(std::vector<std::string> keys, ValuesCallback callback) {
  if (!_connection) {
    callback(_not_connected_error(), {});
    return;
  }
  if (keys.empty()) {
    callback(RedisError{}, {});
    return;
  }

  auto logger = _logger;
  auto conn = _connection;
  auto req = std::make_shared<boost::redis::request>();
  req->push_range("MGET", keys);
  auto resp = std::make_shared<boost::redis::response<std::vector<std::optional<std::string>>>>();

  // boost::redis::connection is not thread safe: every operation starts on its executor.
  boost::asio::post(_io_context, [conn, req, resp, logger, callback = std::move(callback)]() mutable {
    conn->async_exec(*req, *resp, [logger, req, resp, callback = std::move(callback)](boost::system::error_code ec, std::size_t) mutable {
      if (ec) {
        logger->error("Error during: MGET: " + ec.message());
        return callback(RedisError(ec), {});
      }

      auto& result = std::get<0>(*resp);
      if (!result.has_value()) {
        auto error = make_redis_response_error(result.error().diagnostic);
        logger->error("Error during: MGET: " + error.message());
        return callback(std::move(error), {});
      }

      callback(RedisError{}, std::move(result.value()));
    });
  });
}

void RedisDatastore::_read_index(std::string index, std::function<std::string(const std::string&)> key_of, RecordsCallback callback) {
  _async_smembers(index, [this, index, key_of = std::move(key_of), callback = std::move(callback)](RedisError error, std::vector<std::string> members) mutable {
    if (error) return callback(std::move(error), {});

    std::vector<std::string> keys;
    keys.reserve(members.size());
    for (const auto& member : members) keys.push_back(key_of(member));

    _async_mget(keys, [this, index, members, callback = std::move(callback)](RedisError error, std::vector<std::optional<std::string>> values) mutable {
      if (error) return callback(std::move(error), {});

      std::vector<std::pair<std::string, std::string>> records;
      for (std::size_t i = 0; i < members.size() && i < values.size(); ++i) {
        if (values[i]) {
          records.emplace_back(members[i], std::move(*values[i]));
        } else {
          _async_srem(index, members[i], [](RedisError, bool) {});
        }
      }
      callback(RedisError{}, std::move(records));
    });
  });
}

std::string RedisDatastore::_realm_key(const std::string& realm_name) { return "athena:realm:" + realm_name; }

std::string RedisDatastore::_call_key(const std::string& call_id) { return "athena:call:" + call_id; }

std::string RedisDatastore::_realm_index_key() { return "athena:index:realms"; }

std::string RedisDatastore::_subscriber_index_key(const std::string& realm_name) { return "athena:index:subscribers:" + realm_name; }

std::string RedisDatastore::_location_index_key(std::uint64_t subscriber_id) { return "athena:index:locations:" + std::to_string(subscriber_id); }

std::string RedisDatastore::_call_index_key() { return "athena:index:calls"; }

std::string RedisDatastore::_subscriber_key(const std::string& realm_name, const std::string& user) { return "athena:subscriber:" + realm_name + ":" + user; }

std::string RedisDatastore::_location_key(std::uint64_t subscriber_id, const std::string& user, const std::string& host, std::uint16_t port) {
  return "athena:location:" + std::to_string(subscriber_id) + ":" + user + ":" + host + ":" + std::to_string(port);
}

std::string RedisDatastore::_nonce_key(const std::string& nonce) { return "athena:nonce:" + nonce; }

std::string RedisDatastore::_user_key(const std::string& username) { return "athena:user:" + types::User::normalise(username); }

std::string RedisDatastore::_session_key(const std::string& token_hash) { return "athena:session:" + token_hash; }

std::string RedisDatastore::_user_index_key() { return "athena:index:users"; }

std::string RedisDatastore::_trunk_key(const std::string& name) { return "athena:trunk:" + types::Trunk::normalise(name); }

std::string RedisDatastore::_trunk_index_key() { return "athena:index:trunks"; }

std::string RedisDatastore::_session_index_key(const std::string& username) { return "athena:index:sessions:" + types::User::normalise(username); }

std::string RedisDatastore::_duration_ms(std::chrono::high_resolution_clock::time_point start) const {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(3)
      << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start).count() / 1000.0;
  return oss.str();
}

RedisError RedisDatastore::_not_connected_error() const {
  return RedisError(boost::system::errc::make_error_code(boost::system::errc::not_connected), "Redis connection is not started");
}

}  // namespace athenasip::datastores
