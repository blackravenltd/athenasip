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

// Issuing a session and resolving one back to the user that holds it.
//
// This is the half of admin authentication the datastore contract deliberately does not
// know about. A driver holds a session by its hash and prunes it on the absolute expiry
// written on the record; it is never told the idle timeout, it never sees a token, and
// it does not decide whether a login is good. All of that is here - see
// docs/authentication.md.
//
// Nothing here runs on the Core strand. It goes to the datastore on the API's own
// executor, the way ProvisioningAPI does, because an admin logging in must not be able
// to hold up a call.
class Sessions : public std::enable_shared_from_this<Sessions> {
 public:
  // Where the sense of time comes from. Idle expiry is the first rule in this tree that
  // is awkward to test against a real clock - a session that dies after an hour unused
  // takes an hour to watch - so the clock is a seam, as TimerSource is for the section
  // 17 timers. The datastore drivers go on calling std::time themselves: the only thing
  // they do with the time is refuse or prune a record whose expiry has passed, and a
  // test controls that by choosing the expiry.
  using Clock = std::function<std::time_t()>;

  struct Lifetimes {
    // How long a login is good for at all. It is written on the record and is what the
    // store prunes on, so it is never zero.
    std::time_t absolute = 12 * 60 * 60;

    // How long a session survives without being used. Zero turns it off, and then
    // last_seen_at stops being written because nothing reads it.
    std::time_t idle = 60 * 60;
  };

  // Why a login or a lookup produced no identity. The caller turns this into a status
  // code; nothing here knows about HTTP.
  enum class Outcome {
    ok,

    // No such user, the wrong password, a disabled user, an unknown token, an expired
    // one. Deliberately one answer, because between them the alternatives are a list of
    // who holds an account on this node.
    refused,

    // The store could not be asked, which is not the caller's fault and not a 401.
    unavailable,
  };

  template <typename T>
  struct Answer {
    Outcome outcome = Outcome::unavailable;

    // For the log and for nothing else. What a refused client is told is that it was
    // refused.
    std::string reason;

    T value{};

    bool ok() const { return outcome == Outcome::ok; }

    static Answer success(T value) { return Answer{Outcome::ok, {}, std::move(value)}; }
    static Answer refused(std::string reason) { return Answer{Outcome::refused, std::move(reason), T{}}; }
    static Answer unavailable(std::string reason) { return Answer{Outcome::unavailable, std::move(reason), T{}}; }
  };

  // What a login hands back. The token is here once and is never retrievable again from
  // anywhere: what is stored is its SHA-256.
  struct Issued {
    std::string token;
    std::time_t expires_at = 0;
    std::vector<std::string> roles;
  };

  // Who a presented token is. The roles are read off the user rather than copied from
  // the session, so a role removed takes effect on the next request rather than at the
  // next login.
  struct Identity {
    std::shared_ptr<types::User> user;
    std::time_t expires_at = 0;
  };

  using Login = Answer<Issued>;
  using Lookup = Answer<Identity>;

  Sessions(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<datastores::Datastore> datastore, plugins::Executor executor, Lifetimes lifetimes,
           Clock clock = nullptr);

  // A username and password for a token. Refused for a user that does not exist, is
  // disabled, or whose password does not verify, all with the same answer.
  void login(std::string username, std::string password, std::function<void(Login)> handler);

  // A token back to the user that holds it, moving the idle window along as it goes.
  void resolve(std::string token, std::function<void(Lookup)> handler);

  // Ends the presented session. A token naming nothing is success: the state the caller
  // asked for is that the session is gone, and it is.
  void logout(std::string token, plugins::StatusHandler handler);

  // What is stored for a token, and the only form of it anything keeps. No salt: the
  // token is 32 bytes from a CSPRNG, so there is no dictionary to build against it.
  static std::string token_hash(const std::string& token);

 private:
  // 32 bytes from the CSPRNG, hex. Empty when the CSPRNG failed, which is reported
  // rather than worked around: a token that is not random is not a token.
  static std::string _new_token();

  // Remembers that this user logged in. A read hands back a copy, so the copy is what
  // is changed and written back.
  void _record_login(std::shared_ptr<types::User> user, std::time_t now);

  // Moves the idle window along, and not on every request - see the body.
  void _touch(const types::Session& session, std::time_t now);

  std::shared_ptr<loggers::Logger> _logger;
  std::shared_ptr<datastores::Datastore> _datastore;
  plugins::Executor _executor;
  Lifetimes _lifetimes;
  Clock _clock;
};

}  // namespace athenasip::api
