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
#include <utility>
#include <vector>

#include "../datastores/datastore.h"
#include "../loggers/logger.h"
#include "../plugins/plugin.h"
#include "../types/session.h"
#include "../types/user.h"

namespace athenasip::api {

// Issues sessions and resolves a token back to the user holding it
// (docs/authentication.md). The datastore only stores a session by its hash and prunes it
// at its absolute expiry; idle timeout, tokens and whether a login is good are decided
// here. Runs on the API's executor, never the Core strand.
class Sessions : public std::enable_shared_from_this<Sessions> {
 public:
  // Injectable, so a test can pass idle time without waiting. The datastore drivers use
  // std::time themselves; a test controls them by choosing the expiry.
  using Clock = std::function<std::time_t()>;

  struct Lifetimes {
    // How long a login is good for at all. Written on the record and what the store prunes
    // on, so never zero.
    std::time_t absolute = 12 * 60 * 60;

    // How long a session survives unused. Zero turns idle expiry off, and last_seen_at is
    // then not written.
    std::time_t idle = 60 * 60;
  };

  // Why a login or lookup produced no identity. The caller maps it to a status code.
  enum class Outcome {
    ok,

    // No such user, wrong password, disabled user, unknown or expired token. One answer
    // for all, so that a refusal does not reveal who is a user.
    refused,

    // The store could not be asked: not a 401.
    unavailable,
  };

  template <typename T>
  struct Answer {
    Outcome outcome = Outcome::unavailable;

    // For the log only; a refused client is told nothing more than that.
    std::string reason;

    T value{};

    bool ok() const { return outcome == Outcome::ok; }

    static Answer success(T value) { return Answer{Outcome::ok, {}, std::move(value)}; }
    static Answer refused(std::string reason) { return Answer{Outcome::refused, std::move(reason), T{}}; }
    static Answer unavailable(std::string reason) { return Answer{Outcome::unavailable, std::move(reason), T{}}; }
  };

  // What a login returns. The token cannot be retrieved again: only its SHA-256 is stored.
  struct Issued {
    std::string token;
    std::time_t expires_at = 0;
    std::vector<std::string> roles;
  };

  // Who a presented token is. Roles are read from the user, not the session, so removing a
  // role takes effect on the next request.
  struct Identity {
    std::shared_ptr<types::User> user;
    std::time_t expires_at = 0;
  };

  using Login = Answer<Issued>;
  using Lookup = Answer<Identity>;

  Sessions(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor, Lifetimes lifetimes,
           Clock clock = nullptr);

  // Exchanges a username and password for a token. A missing user, a disabled one and a
  // wrong password are refused identically.
  void login(std::string username, std::string password, std::function<void(Login)> handler);

  // Resolves a token to the user holding it, and extends the idle window.
  void resolve(std::string token, std::function<void(Lookup)> handler);

  // Ends the presented session. A token naming nothing is success.
  void logout(std::string token, plugins::StatusHandler handler);

  // The stored form of a token. Unsalted: the token is 32 bytes from a CSPRNG, so there is
  // no dictionary to build against it.
  static std::string token_hash(const std::string& token);

 private:
  // 32 bytes from the CSPRNG, as hex. Empty when the CSPRNG failed.
  static std::string _new_token();

  // Records the login time on the user and writes it back.
  void _record_login(std::shared_ptr<types::User> user, std::time_t now);

  // Extends the idle window, though not on every request - see the body.
  void _touch(const types::Session& session, std::time_t now);

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<datastores::Datastore> _datastore;
  plugins::Executor _executor;
  Lifetimes _lifetimes;
  Clock _clock;
};

}  // namespace athenasip::api
