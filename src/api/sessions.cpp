//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sessions.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>

#include "../loggers/logger_scoped.h"
#include "../types/password.h"
#include "../util.h"

namespace athenasip::api {

namespace {

constexpr std::size_t kTokenBytes = 32;

}  // namespace

Sessions::Sessions(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor, Lifetimes lifetimes,
                   Clock clock)
    : _logger(std::make_shared<loggers::LoggerScoped>("sessions", std::move(logger))),
      _datastore(std::move(datastore)),
      _executor(std::move(executor)),
      _lifetimes(lifetimes),
      _clock(clock ? std::move(clock) : Clock([] { return std::time(nullptr); })) {}

std::string Sessions::token_hash(const std::string& token) { return token.empty() ? std::string() : Util::sha256(token); }

std::string Sessions::_new_token() {
  std::array<std::uint8_t, kTokenBytes> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return {};

  // Hex rather than base64: nothing to escape in a header, a URL or a shell.
  return Util::to_hex(bytes.data(), static_cast<std::uint16_t>(bytes.size()));
}

void Sessions::login(std::string username, std::string password, std::function<void(Login)> handler) {
  if (username.empty() || password.empty()) return handler(Login::refused("no username or no password"));

  auto self = shared_from_this();

  _datastore->user_get(_executor, types::User::normalise(username), [self, password, handler](plugins::Result<std::shared_ptr<types::User>> result) {
    if (!result.ok) return handler(Login::unavailable(result.error));

    auto user = result.value;

    // One answer for all three. Timing still differs - an unknown user skips the PBKDF2 -
    // which the login rate limit bounds.
    if (!user) return handler(Login::refused("no such user"));
    if (user->disabled) return handler(Login::refused("the user is disabled"));
    if (!types::Password::verify(password, user->password_hash)) return handler(Login::refused("the password does not verify"));

    const auto token = _new_token();
    if (token.empty()) return handler(Login::unavailable("the CSPRNG failed"));

    const auto now = self->_clock();

    types::Session session;
    session.token_hash = token_hash(token);
    session.username = user->key();
    session.created_at = now;
    session.expires_at = now + self->_lifetimes.absolute;
    session.last_seen_at = now;

    const auto expires_at = session.expires_at;
    const auto roles = user->roles;

    self->_datastore->session_create(self->_executor, session, [self, handler, token, expires_at, roles, user, now](plugins::Status status) {
      if (!status.ok) return handler(Login::unavailable(status.error));

      self->_record_login(user, now);
      self->_logger->info("login: " + user->key() + " holds a session until " + Util::to_iso8601(expires_at));

      handler(Login::success(Issued{token, expires_at, roles}));
    });
  });
}

void Sessions::resolve(std::string token, std::function<void(Lookup)> handler) {
  if (token.empty()) return handler(Lookup::refused("no token"));

  auto self = shared_from_this();
  const auto hash = token_hash(token);

  _datastore->session_get(_executor, hash, [self, handler, hash](plugins::Result<std::shared_ptr<types::Session>> result) {
    if (!result.ok) return handler(Lookup::unavailable(result.error));

    auto session = result.value;
    if (!session) return handler(Lookup::refused("no such session"));

    const auto now = self->_clock();

    if (session->has_expired(now, self->_lifetimes.idle)) {
      // The store prunes on absolute expiry only, so an idle-expired session is deleted
      // here.
      self->_datastore->session_delete(self->_executor, hash, [self](plugins::Status status) {
        if (!status.ok) self->_logger->warn("could not delete an expired session: " + status.error);
      });

      return handler(Lookup::refused("the session has expired"));
    }

    self->_datastore->user_get(self->_executor, session->username, [self, handler, session](plugins::Result<std::shared_ptr<types::User>> result) {
      if (!result.ok) return handler(Lookup::unavailable(result.error));

      auto user = result.value;

      // A session that outlives its user is not an identity. Its deletion belongs with
      // deleting or disabling the user.
      if (!user) return handler(Lookup::refused("the user this session names is gone"));
      if (user->disabled) return handler(Lookup::refused("the user is disabled"));

      self->_touch(*session, self->_clock());

      handler(Lookup::success(Identity{user, session->expires_at}));
    });
  });
}

void Sessions::logout(std::string token, plugins::StatusHandler handler) {
  if (token.empty()) return handler(plugins::Status::success());

  _datastore->session_delete(_executor, token_hash(token), std::move(handler));
}

void Sessions::_record_login(std::shared_ptr<types::User> user, std::time_t now) {
  user->last_login_at = now;

  auto self = shared_from_this();
  _datastore->user_update(_executor, user, [self](plugins::Status status) {
    // A failed bookkeeping write does not undo the login, but is worth a warning: the store
    // may be refusing writes.
    if (!status.ok) self->_logger->warn("could not record the login time: " + status.error);
  });
}

void Sessions::_touch(const types::Session& session, std::time_t now) {
  // Nothing reads last_seen_at without an idle rule.
  if (_lifetimes.idle <= 0) return;

  // Refreshed once per tenth of the idle window rather than on every request, so that an
  // authenticated read does not become a datastore write.
  const auto threshold = std::max<std::time_t>(1, _lifetimes.idle / 10);
  if (now - session.last_seen_at < threshold) return;

  auto refreshed = session;
  refreshed.last_seen_at = now;

  auto self = shared_from_this();

  // The contract has no session_update: creating over a held hash replaces the record.
  _datastore->session_create(_executor, refreshed, [self](plugins::Status status) {
    if (!status.ok) self->_logger->warn("could not move the idle window along: " + status.error);
  });
}

}  // namespace athenasip::api
