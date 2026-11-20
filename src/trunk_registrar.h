//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <boost/json.hpp>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "events/event_system.h"
#include "loggers/logger.h"
#include "loggers/logger_scoped.h"
#include "sip_message.h"
#include "timer_source.h"
#include "types/authorization.h"
#include "types/trunk.h"

namespace athenasip {

class Core;
class Channel;

// RFC 3261 10 as a client: this node registers to each trunk that asks for it, so the carrier knows where to send
// calls. One node of a cluster does, by a lease in the datastore; another takes over when the lease lapses. The
// carrier's challenges are answered with the trunk's credentials, the registration is refreshed before it runs
// out, and a failure is retried with backoff. Each trunk's state is a record in the datastore (trunk_status_set), which
// the API on any node reads, and goes on the bus as trunks/<name>/status to say it changed.
class TrunkRegistrar : public std::enable_shared_from_this<TrunkRegistrar> {
 public:
  // How often the trunks are read again, and how long the lease on each is held between readings.
  static constexpr std::chrono::seconds kScan{30};
  static constexpr std::uint32_t kLeaseSeconds = 90;

  struct Status {
    // "registering", "registered" or "failed".
    std::string state;

    // Why it failed, or the response that answered, for an operator.
    std::string detail;

    // When the carrier's binding lapses, for a registered trunk.
    std::time_t expires_at = 0;
  };

  TrunkRegistrar(std::shared_ptr<loggers::Logger> logger, std::weak_ptr<Core> core);

  // Reads the trunks now, every kScan after, and whenever the bus says one changed (trunks/changed). On the strand.
  void start();
  void stop();

  // Reads the trunks now, as start does and each scan does. The next scheduled scan is kScan from now.
  void scan();

  // The trunks this node registers, by name.
  std::map<std::string, Status> statuses() const;

  // Whether user is the user part of a Contact this node registers to a trunk, so what the carrier sends there,
  // such as an OPTIONS keepalive, is addressed to this node and not to a subscriber. On the strand.
  bool registered_contact(const std::string& user) const;

 private:
  struct Registration {
    types::Trunk trunk;

    // RFC 3261 10.2: one Call-ID for every REGISTER to the same registrar, and a CSeq that rises.
    std::string call_id;
    std::string from_tag;
    std::uint64_t cseq = 0;

    // What the carrier asked for after a 423, else the trunk's.
    std::uint32_t expires = 0;

    std::shared_ptr<Timer> timer;
    std::uint32_t failures = 0;
    bool in_flight = false;

    // Sending Expires 0, and gone once it is answered.
    bool leaving = false;

    Status status;
  };

  void _schedule_scan();
  void _unlisten();
  void _keep(const types::Trunk& trunk);
  // Stops registering. With unregister, the carrier's binding is removed first (Expires 0, RFC 3261 10.2.2): the
  // trunk no longer asks to be registered. Without, another node has the lease and the binding is now its.
  void _drop(const std::string& name, bool unregister);

  void _gone(const std::string& name);
  void _register(const std::string& name);
  void _send(const std::string& name, const std::shared_ptr<Channel>& channel, const std::optional<types::Authorization>& credentials, bool proxy_auth);
  void _on_answer(const std::string& name, const std::shared_ptr<Channel>& channel, const std::shared_ptr<SIPMessage>& request,
                  const std::shared_ptr<SIPMessage>& response, bool answered_challenge);
  void _failed(const std::string& name, const std::string& why);
  void _after(const std::string& name, std::chrono::seconds delay);
  void _publish(const std::string& name);

  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::weak_ptr<Core> _core;
  std::map<std::string, Registration> _registrations;
  std::shared_ptr<Timer> _scan_timer;
  std::shared_ptr<events::Subscription> _changed;
  bool _running = false;
};

}  // namespace athenasip
