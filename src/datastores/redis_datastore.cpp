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
  location.account_id = json_uint64(obj, "account_id");
  location.contact = std::make_shared<types::SIPUri>(json_string(obj, "contact"));
  location.registered_at = static_cast<std::time_t>(json_uint64(obj, "registered_at"));
  location.expires_at = static_cast<std::time_t>(json_uint64(obj, "expires_at"));
  location.nat = json_string(obj, "nat") == "Y";

  if (obj.if_contains("node_id")) location.node_id = json_string(obj, "node_id");
  if (obj.if_contains("flow_id")) location.flow_id = json_string(obj, "flow_id");
  if (obj.if_contains("path")) location.path = json_string(obj, "path");

  return location;
}

// A list operation is a set read followed by a read per member, and with no blocking
// primitive left there is nothing to loop over: each step has to start the next one
// from its own completion. This walks the members in order and calls done() at the
// end. Pipelining them would be faster and is a later optimisation; what matters here
// is that none of it holds up the caller.
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
    // The library's chatter goes through this node's logger rather than its own, which
    // writes to stderr unscoped and at no level - and into the output of
    // `athenasip --add-user`, whose whole point is a clean answer. Its info is connection
    // lifecycle, which is this node's debug; anything it calls a warning or worse keeps
    // that weight here. The logger is held by value because the library calls it from
    // the IO thread, possibly after this object has begun to close.
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

  // The first PING is what turns "the thread started" into "Redis answered".
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

    // Not found is a success carrying nothing. The registrar answers 404 to that and
    // 500 to a failure, so the two cannot be the same answer.
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

  // create is not update: an existing realm is a conflict.
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

void RedisDatastore::realm_delete(plugins::Executor on, std::string realm_name, plugins::StatusHandler handler) {
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

  _async_smembers(_realm_index_key(), [this, on, handler](RedisError error, std::vector<std::string> names) mutable {
    if (error) return _complete(on, handler, Answer::failure(error.message()));

    auto realms = std::make_shared<std::vector<std::shared_ptr<types::Realm>>>();

    run_sequence(
        std::move(names),
        [this, realms](std::string name, std::function<void()> next) {
          _async_get(_realm_key(name), [this, realms, next](RedisError error, std::optional<std::string> value) mutable {
            if (!error && value) {
              try {
                realms->push_back(_parse_realm(*value));
              } catch (const std::exception& ex) {
                _logger->error("realm_list: " + std::string(ex.what()));
              }
            }

            next();
          });
        },
        [this, on, handler, realms]() { _complete(on, handler, Answer::success(std::move(*realms))); });
  });
}

void RedisDatastore::user_get(plugins::Executor on, std::string username, plugins::Handler<std::shared_ptr<types::User>> handler) {
  using Answer = plugins::Result<std::shared_ptr<types::User>>;

  _async_get(_user_key(username), [this, on, handler](RedisError error, std::optional<std::string> value) mutable {
    if (error) return _complete(on, handler, Answer::failure(error.message()));

    // Not found is a success carrying nothing, as everywhere else: the API answers 404
    // to that and 500 to a failure, and they cannot be the same answer.
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

  // create is not update: an existing username is a conflict, and the key is case-folded
  // so "Tom" and "tom" are the same conflict rather than two logins nobody can tell
  // apart.
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

      // A live token against a user that no longer exists is a session nobody can
      // revoke, so the sessions go with the user. Whether there was a user to delete is
      // already settled by the DEL above and is not changed by how many sessions it held.
      _session_delete_all_for(name, [this, on, handler, removed](RedisError error) mutable {
        if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
        _complete(on, handler, _status(removed > 0, "user_delete"));
      });
    });
  });
}

void RedisDatastore::user_list(plugins::Executor on, plugins::Handler<std::vector<std::shared_ptr<types::User>>> handler) {
  using Answer = plugins::Result<std::vector<std::shared_ptr<types::User>>>;

  const auto index = _user_index_key();

  _async_smembers(index, [this, on, handler, index](RedisError error, std::vector<std::string> names) mutable {
    if (error) return _complete(on, handler, Answer::failure(error.message()));

    auto users = std::make_shared<std::vector<std::shared_ptr<types::User>>>();

    run_sequence(
        std::move(names),
        [this, users, index](std::string name, std::function<void()> next) {
          _async_get(_user_key(name), [this, users, index, name, next](RedisError error, std::optional<std::string> value) mutable {
            if (error) return next();

            // An index member whose record is gone is stale rather than fatal, and is
            // removed on the way past, as call_list does.
            if (!value) return _async_srem(index, name, [next](RedisError, bool) mutable { next(); });

            try {
              users->push_back(_parse_user(*value));
            } catch (const std::exception& ex) {
              _logger->error("user_list: skipping " + name + ": " + std::string(ex.what()));
            }

            next();
          });
        },
        [this, on, handler, users]() { _complete(on, handler, Answer::success(std::move(*users))); });
  });
}

// Writing a hash that is already held replaces it, which is how last_seen_at moves:
// there is no session_update on the contract because a token hash is 32 bytes from a
// CSPRNG and does not collide by accident.
//
// The record expires on its own absolute expiry, which is what makes that expiry the
// store's to keep. Idle expiry is not here: how long a session survives unused is
// configuration this driver is not given, and the caller asks Session::has_expired.
void RedisDatastore::session_create(plugins::Executor on, types::Session session, plugins::StatusHandler handler) {
  if (session.token_hash.empty()) return _complete(on, handler, plugins::Status::failure("session_create: no token hash"));
  if (session.username.empty()) return _complete(on, handler, plugins::Status::failure("session_create: no user"));

  const auto now = std::time(nullptr);
  if (session.expires_at <= now) {
    _logger->warn("session_create: refusing to create already-expired session");
    return _complete(on, handler, plugins::Status::failure("session_create: already expired"));
  }

  // Filed under the same key the user is, so revoking by username finds these whatever
  // case the login was typed in.
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
  // The record says whose it is, and that is the only way to reach the index entry
  // without KEYS. A session already gone leaves its hash in the index, which the revoke
  // and listing paths remove on the way past.
  session_get(on, token_hash, [this, on, handler, token_hash](plugins::Result<std::shared_ptr<types::Session>> found) mutable {
    if (!found.ok) return _complete(on, handler, plugins::Status::failure(found.error));

    const auto username = found.value ? found.value->username : std::string();

    // Gone either way, as the contract says: whether the key was there is not something
    // the answer may reveal, because the caller identified it with a secret it presented.
    // Only Redis refusing counts as a failure.
    _async_del(_session_key(token_hash), [this, on, handler, token_hash, username](RedisError error, std::int64_t) mutable {
      if (error) return _complete(on, handler, plugins::Status::failure(error.message()));

      if (username.empty()) return _complete(on, handler, plugins::Status::success());

      _async_srem(_session_index_key(username), token_hash, [this, on, handler](RedisError error, bool) mutable {
        _complete(on, handler, error ? plugins::Status::failure(error.message()) : plugins::Status::success());
      });
    });
  });
}

// Nothing to revoke is not a failure: the caller asked for this user to hold no sessions,
// and it holds none. That is what makes DELETE /users/{u}/sessions a 204 for a user who
// has never logged in, with the 404 for an unknown user left to the handler.
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
          // The index goes too, rather than being emptied member by member: it holds
          // nothing that outlives the sessions it pointed at.
          _async_del(index, [done](RedisError error, std::int64_t) mutable { done(error); });
        });
  });
}

void RedisDatastore::account_get(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity,
                                 plugins::Handler<std::shared_ptr<types::Account>> handler) {
  using Answer = plugins::Result<std::shared_ptr<types::Account>>;

  if (!identity || !identity->uri) return _complete(on, handler, Answer::failure("account_get: no identity"));

  _async_get(_account_key(identity->uri->host, identity->uri->user), [this, on, handler, identity](RedisError error, std::optional<std::string> value) mutable {
    if (error) return _complete(on, handler, Answer::failure(error.message()));
    if (!value) return _complete(on, handler, Answer::success(nullptr));

    try {
      const auto parsed = boost::json::parse(*value);
      const auto& obj = parsed.as_object();

      auto account = std::make_shared<types::Account>();
      account->id = json_uint64(obj, "id");
      account->identity = std::move(identity);
      account->ha1 = json_string(obj, "ha1");

      // Optional: an account imported as a bare MD5 hash has no SHA-256 credential, and
      // a missing field is that rather than a broken row.
      if (obj.if_contains("ha1_sha256")) account->ha1_sha256 = json_string(obj, "ha1_sha256");
      _complete(on, handler, Answer::success(std::move(account)));
    } catch (const std::exception& ex) {
      _logger->error("account_get: " + std::string(ex.what()));
      _complete(on, handler, Answer::failure(ex.what()));
    }
  });
}

void RedisDatastore::account_create(plugins::Executor on, std::shared_ptr<types::Account> account, plugins::StatusHandler handler) {
  if (!account || !account->identity || !account->identity->uri) return _complete(on, handler, plugins::Status::failure("account_create: no account"));

  const auto& uri = account->identity->uri;
  const auto key = _account_key(uri->host, uri->user);
  const auto index = _account_index_key(uri->host);
  const auto user = uri->user;
  const auto body = _serialise_account(account);

  _async_exists(key, [this, on, handler, key, index, user, body](RedisError error, bool exists) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
    if (exists) return _complete(on, handler, plugins::Status::failure("account_create: " + user + " already exists"));

    _async_set(key, body, [this, on, handler, index, user](RedisError error, bool ok) mutable {
      if (error || !ok) return _complete(on, handler, plugins::Status::failure(error ? error.message() : "account_create: SET failed"));

      _async_sadd(index, user, [this, on, handler](RedisError error, bool) mutable {
        _complete(on, handler, error ? plugins::Status::failure(error.message()) : plugins::Status::success());
      });
    });
  });
}

void RedisDatastore::account_update(plugins::Executor on, std::shared_ptr<types::Account> account, plugins::StatusHandler handler) {
  if (!account || !account->identity || !account->identity->uri) return _complete(on, handler, plugins::Status::failure("account_update: no account"));

  const auto& uri = account->identity->uri;
  const auto key = _account_key(uri->host, uri->user);
  const auto user = uri->user;
  const auto body = _serialise_account(account);

  _async_exists(key, [this, on, handler, key, user, body](RedisError error, bool exists) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
    if (!exists) return _complete(on, handler, plugins::Status::failure("account_update: " + user + " does not exist"));

    _async_set(key, body, [this, on, handler](RedisError error, bool ok) mutable {
      _complete(on, handler, !error && ok ? plugins::Status::success() : plugins::Status::failure(error ? error.message() : "account_update: SET failed"));
    });
  });
}

void RedisDatastore::account_delete(plugins::Executor on, std::shared_ptr<types::SIPIdentity> identity, plugins::StatusHandler handler) {
  if (!identity || !identity->uri) return _complete(on, handler, plugins::Status::failure("account_delete: no identity"));

  const auto realm_name = identity->uri->host;
  const auto user = identity->uri->user;

  // The bindings go with the account, so the record has to be read before it is
  // deleted: the location index is keyed on the account id.
  account_get(on, identity, [this, on, handler, realm_name, user](plugins::Result<std::shared_ptr<types::Account>> found) mutable {
    if (!found.ok) return _complete(on, handler, plugins::Status::failure(found.error));

    auto account = found.value;

    _async_del(_account_key(realm_name, user), [this, on, handler, realm_name, user, account](RedisError error, std::int64_t removed) mutable {
      if (error) return _complete(on, handler, plugins::Status::failure(error.message()));

      _async_srem(_account_index_key(realm_name), user, [this, on, handler, account, removed](RedisError error, bool) mutable {
        if (error) return _complete(on, handler, plugins::Status::failure(error.message()));

        if (!account) return _complete(on, handler, _status(removed > 0, "account_delete"));

        // A deleted account keeps no bindings.
        const auto index = _location_index_key(account->id);

        _async_smembers(index, [this, on, handler, index, removed](RedisError error, std::vector<std::string> keys) mutable {
          if (error) return _complete(on, handler, plugins::Status::failure(error.message()));

          run_sequence(
              std::move(keys), [this](std::string key, std::function<void()> next) { _async_del(key, [next](RedisError, std::int64_t) mutable { next(); }); },
              [this, on, handler, index, removed]() {
                _async_del(index, [this, on, handler, removed](RedisError error, std::int64_t) mutable {
                  if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
                  _complete(on, handler, _status(removed > 0, "account_delete"));
                });
              });
        });
      });
    });
  });
}

void RedisDatastore::account_list(plugins::Executor on, std::string realm_name, plugins::Handler<std::vector<std::shared_ptr<types::Account>>> handler) {
  using Answer = plugins::Result<std::vector<std::shared_ptr<types::Account>>>;

  _async_smembers(_account_index_key(realm_name), [this, on, handler, realm_name](RedisError error, std::vector<std::string> users) mutable {
    if (error) return _complete(on, handler, Answer::failure(error.message()));

    auto accounts = std::make_shared<std::vector<std::shared_ptr<types::Account>>>();

    run_sequence(
        std::move(users),
        [this, on, accounts, realm_name](std::string user, std::function<void()> next) {
          auto identity = std::make_shared<types::SIPIdentity>("sip:" + user + "@" + realm_name);

          account_get(on, std::move(identity), [accounts, next](plugins::Result<std::shared_ptr<types::Account>> found) mutable {
            if (found.ok && found.value) accounts->push_back(found.value);
            next();
          });
        },
        [this, on, handler, accounts]() { _complete(on, handler, Answer::success(std::move(*accounts))); });
  });
}

void RedisDatastore::account_register(plugins::Executor on, std::shared_ptr<types::Account> account, types::Location binding, std::uint32_t expires_seconds,
                                      plugins::StatusHandler handler) {
  if (!account || !binding.contact) return _complete(on, handler, plugins::Status::failure("account_register: no account or contact"));

  const auto contact = binding.contact;
  const std::uint16_t port = contact->port.value_or(0);
  const auto now = static_cast<std::int64_t>(std::time(nullptr));
  const bool is_nat = Util::is_ipv4(contact->host) && Util::is_ipv4_private(contact->host);

  constexpr std::int64_t kDefaultRegistrationSeconds = 3600;

  // The registrar negotiated this lifetime and told the client about it, so it is what
  // Redis expires the binding on.
  const std::int64_t ttl = expires_seconds > 0 ? static_cast<std::int64_t>(expires_seconds) : kDefaultRegistrationSeconds;

  boost::json::object location;
  location["account_id"] = account->id;
  location["contact"] = contact->to_string();
  location["user"] = contact->user;
  location["host"] = contact->host;
  location["port"] = port;
  location["registered_at"] = now;
  location["expires_at"] = now + ttl;
  location["nat"] = is_nat ? "Y" : "N";
  if (!binding.path.empty()) location["path"] = binding.path;

  // What a second node needs to use this binding: which flow it was learned over and who
  // holds it. Empty on a single node, which is why they are written only when set.
  if (!binding.flow_id.empty()) location["flow_id"] = binding.flow_id;
  if (!binding.node_id.empty()) location["node_id"] = binding.node_id;

  const auto key = _location_key(account->id, contact->user, contact->host, port);
  const auto index = _location_index_key(account->id);

  _async_set_ex(key, boost::json::serialize(location), std::chrono::seconds(ttl), [this, on, handler, key, index](RedisError error, bool ok) mutable {
    if (error || !ok) return _complete(on, handler, plugins::Status::failure(error ? error.message() : "account_register: SETEX failed"));

    // The index is what location_list reads, so listing never needs KEYS.
    _async_sadd(index, key, [this, on, handler](RedisError error, bool) mutable {
      _complete(on, handler, error ? plugins::Status::failure(error.message()) : plugins::Status::success());
    });
  });
}

void RedisDatastore::account_unregister(plugins::Executor on, std::shared_ptr<types::Account> account, std::shared_ptr<types::SIPUri> contact,
                                        plugins::StatusHandler handler) {
  if (!account || !contact) return _complete(on, handler, plugins::Status::failure("account_unregister: no account or contact"));

  const std::uint16_t port = contact->port.value_or(0);
  const auto key = _location_key(account->id, contact->user, contact->host, port);
  const auto index = _location_index_key(account->id);

  _async_del(key, [this, on, handler, key, index](RedisError error, std::int64_t removed) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));

    _async_srem(index, key, [this, on, handler, removed](RedisError error, bool) mutable {
      if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
      _complete(on, handler, _status(removed > 0, "account_unregister"));
    });
  });
}

void RedisDatastore::location_list(plugins::Executor on, std::uint64_t account_id, plugins::Handler<std::vector<types::Location>> handler) {
  using Answer = plugins::Result<std::vector<types::Location>>;

  const auto index = _location_index_key(account_id);

  _async_smembers(index, [this, on, handler, index](RedisError error, std::vector<std::string> keys) mutable {
    if (error) return _complete(on, handler, Answer::failure(error.message()));

    auto locations = std::make_shared<std::vector<types::Location>>();

    run_sequence(
        std::move(keys),
        [this, locations, index](std::string key, std::function<void()> next) {
          _async_get(key, [this, locations, index, key, next](RedisError error, std::optional<std::string> value) mutable {
            if (error) return next();

            // The binding expired and Redis dropped it; tidy the index as we go.
            if (!value) {
              return _async_srem(index, key, [next](RedisError, bool) mutable { next(); });
            }

            try {
              locations->push_back(parse_location(*value));
            } catch (const std::exception& ex) {
              // One unreadable binding must not hide the others: target determination
              // needs every contact it can get (RFC 3261 16.5).
              _logger->error("location_list: skipping " + key + ": " + std::string(ex.what()));
            }

            next();
          });
        },
        [this, on, handler, locations]() { _complete(on, handler, Answer::success(std::move(*locations))); });
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

  _async_exists(key, [this, on, handler, key, id, body](RedisError error, bool exists) mutable {
    if (error) return _complete(on, handler, plugins::Status::failure(error.message()));
    if (!exists) return _complete(on, handler, plugins::Status::failure("call_update: " + id + " does not exist"));

    _async_set(key, body, [this, on, handler](RedisError error, bool ok) mutable {
      _complete(on, handler, !error && ok ? plugins::Status::success() : plugins::Status::failure(error ? error.message() : "call_update: SET failed"));
    });
  });
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

  const auto index = _call_index_key();

  _async_smembers(index, [this, on, handler, index](RedisError error, std::vector<std::string> ids) mutable {
    if (error) return _complete(on, handler, Answer::failure(error.message()));

    auto calls = std::make_shared<std::vector<std::shared_ptr<Call>>>();

    run_sequence(
        std::move(ids),
        [this, calls, index](std::string id, std::function<void()> next) {
          _async_get(_call_key(id), [this, calls, index, id, next](RedisError error, std::optional<std::string> value) mutable {
            if (error) return next();

            if (!value) {
              return _async_srem(index, id, [next](RedisError, bool) mutable { next(); });
            }

            try {
              calls->push_back(_parse_call(*value));
            } catch (const std::exception& ex) {
              _logger->error("call_list: skipping " + id + ": " + std::string(ex.what()));
            }

            next();
          });
        },
        [this, on, handler, calls]() { _complete(on, handler, Answer::success(std::move(*calls))); });
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
  obj["media_anchor"] = realm->media.anchor;
  obj["media_profiles"] = types::MediaPolicy::to_string(realm->media.profiles);
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

  // A realm written before the policy existed has neither field, and the defaults are
  // what that realm was already doing.
  if (const auto* anchor = obj.if_contains("media_anchor"); anchor != nullptr && anchor->is_bool()) realm->media.anchor = anchor->as_bool();
  realm->media.profiles = types::MediaPolicy::profiles_from_string(json_string(obj, "media_profiles"), realm->media.profiles);

  return realm;
}

std::string RedisDatastore::_serialise_user(const std::shared_ptr<types::User>& user) {
  boost::json::object obj;

  // The username as given, not the key it is filed under: the key is case-folded and
  // this is what the console displays and what an audit line names.
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

  // A role this build does not know is carried rather than dropped. It grants nothing -
  // every authorisation check asks whether a specific known role is held, so a string
  // nothing recognises can never match one - and dropping it would mean an older node
  // rewriting a user silently strips a role a newer node gave them. Validating what may
  // be granted is the API's job, on the way in.
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

std::string RedisDatastore::_serialise_account(const std::shared_ptr<types::Account>& account) {
  boost::json::object obj;
  obj["id"] = account->id;
  obj["ha1"] = account->ha1;

  // Only when there is one: an account imported as a bare MD5 HA1 has no SHA-256
  // credential, and writing an empty one would make it look like a hash of nothing.
  if (!account->ha1_sha256.empty()) obj["ha1_sha256"] = account->ha1_sha256;
  obj["uri"] = account->identity->uri->to_string();
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

  // Participants are persisted; their media streams are not. A relay set belongs to
  // the node that allocated it and cannot be handed to another one.
  boost::json::array participants;
  for (const auto& participant : call->participants) {
    boost::json::object entry;
    entry["identity"] = participant.identity ? participant.identity->to_string() : "";
    entry["node_id"] = participant.node_id;
    // The dialog's identity, not the whole dialog. A route set and a pair of sequence
    // numbers belong to the node on the path and are no use to another one; replicating
    // them is full dialog failover, which is parked.
    entry["call_id"] = participant.dialog ? participant.dialog->call_id : "";
    entry["caller_tag"] = participant.dialog ? participant.dialog->caller_tag : "";
    entry["callee_tag"] = participant.dialog ? participant.dialog->callee_tag : "";
    entry["originator"] = participant.originator;
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

  if (const auto* participants = obj.if_contains("participants"); participants != nullptr && participants->is_array()) {
    for (const auto& entry : participants->as_array()) {
      const auto& participant_obj = entry.as_object();

      auto& participant = call->add_participant(std::make_shared<types::SIPIdentity>(json_string(participant_obj, "identity")), nullptr,
                                                participant_obj.at("originator").as_bool());
      participant.node_id = json_string(participant_obj, "node_id");

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

  // boost::redis::connection is not thread safe: its channels use a null mutex, so
  // every operation has to be initiated on the connection's own executor rather than
  // on whichever thread happened to call in.
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

  // boost::redis::connection is not thread safe: its channels use a null mutex, so
  // every operation has to be initiated on the connection's own executor rather than
  // on whichever thread happened to call in.
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

  // boost::redis::connection is not thread safe: its channels use a null mutex, so
  // every operation has to be initiated on the connection's own executor rather than
  // on whichever thread happened to call in.
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

  // boost::redis::connection is not thread safe: its channels use a null mutex, so
  // every operation has to be initiated on the connection's own executor rather than
  // on whichever thread happened to call in.
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

std::string RedisDatastore::_realm_key(const std::string& realm_name) { return "athena:realm:" + realm_name; }

std::string RedisDatastore::_call_key(const std::string& call_id) { return "athena:call:" + call_id; }

std::string RedisDatastore::_realm_index_key() { return "athena:index:realms"; }

std::string RedisDatastore::_account_index_key(const std::string& realm_name) { return "athena:index:accounts:" + realm_name; }

std::string RedisDatastore::_location_index_key(std::uint64_t account_id) { return "athena:index:locations:" + std::to_string(account_id); }

std::string RedisDatastore::_call_index_key() { return "athena:index:calls"; }

std::string RedisDatastore::_account_key(const std::string& realm_name, const std::string& user) { return "athena:account:" + realm_name + ":" + user; }

std::string RedisDatastore::_location_key(std::uint64_t account_id, const std::string& user, const std::string& host, std::uint16_t port) {
  return "athena:location:" + std::to_string(account_id) + ":" + user + ":" + host + ":" + std::to_string(port);
}

std::string RedisDatastore::_nonce_key(const std::string& nonce) { return "athena:nonce:" + nonce; }

std::string RedisDatastore::_user_key(const std::string& username) { return "athena:user:" + types::User::normalise(username); }

std::string RedisDatastore::_session_key(const std::string& token_hash) { return "athena:session:" + token_hash; }

std::string RedisDatastore::_user_index_key() { return "athena:index:users"; }

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
