//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "call.h"
#include "loggers/logger.h"
#include "loggers/logger_scoped.h"
#include "sip_message.h"
#include "timer_source.h"
#include "transactions/transaction_base.h"
#include "types/presence_record.h"

namespace athenasip {

class Channel;
class Core;

// The node as an RFC 6665 notifier for the subscribers in its realms (docs/design/presence.md): it answers a
// SUBSCRIBE for a package it serves and sends the NOTIFYs, in a dialog of its own with the watcher. Every
// subscription is a datastore record naming the node and flow that hold the watcher; this node keeps nothing of a
// subscription but its expiry timer, and reads the record before it acts.
class Notifier : public std::enable_shared_from_this<Notifier> {
 public:
  Notifier(std::shared_ptr<loggers::Logger> logger, std::weak_ptr<Core> core);

  // Listens on the bus for presentities whose state changed.
  void start();

  // A presentity's state changed: its watchers held here are sent the new state. Changes within one turn of the
  // strand make one NOTIFY.
  void changed(const std::string& aor);

  // The packages this node is the notifier for, as Allow-Events lists them (RFC 6665 7.2.2).
  static const char* packages() { return "dialog"; }

  // The Request-URI parameter on the Contact the node gives a watcher, naming the subscription. It brings a
  // refresh to the notifier from any node that serves the realm.
  static constexpr const char* kSubscriptionParameter = "athena-sub";

  // Whether an authorised out-of-dialog request is the notifier's rather than the proxy's: a SUBSCRIBE for a package
  // served here, addressed to an address of record (no Route left, no flow token), not forwarded by a peer, with
  // presence on and a datastore that holds it.
  bool wants(const SIPMessage& request) const;

  // A request `wants` took. `otherwise` routes it as before, when the Request-URI names no realm served here.
  void on_subscribe(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, std::function<void()> otherwise);

  // Whether a request is addressed to a Contact this notifier gave: inside a subscription's dialog, and its alone.
  static bool owns(const SIPMessage& request);

  void on_in_dialog(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction);

 private:
  // The subscription dialog (RFC 6665 4.1, RFC 3261 12.1.2), as its record's body holds it.
  struct Subscription {
    std::string id;
    std::string presentity;
    std::string package;
    std::string event_id;

    std::string call_id;
    std::string local_tag;
    std::string remote_tag;

    // The watcher's From, and the SUBSCRIBE's To, as the NOTIFY's To and From.
    std::string watcher_uri;
    std::string presentity_uri;

    std::string remote_target;
    std::vector<std::string> route_set;
    std::string contact;

    std::uint64_t local_cseq = 0;
    std::uint64_t remote_cseq = 0;

    // RFC 4235 4.1: rises by one with every NOTIFY on this subscription.
    std::uint64_t version = 0;

    std::uint32_t expires = 0;
    std::time_t expires_at = 0;

    std::string flow_id;
    std::string node_id;
  };

  struct Expiry {
    std::uint32_t granted = 0;
    std::uint16_t code = 0;  // 0, or the answer that refuses it
  };

  Expiry _expiry(const SIPMessage& request, bool initial) const;

  void _accept(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, std::string presentity, std::string package,
               std::uint32_t expires);
  void _refresh(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, Subscription subscription);

  // Writes the record, then runs `then`: what is answered or sent follows from what is stored.
  void _store(const Subscription& subscription, std::function<void(bool)> then);
  void _end(const Subscription& subscription);

  // A NOTIFY in the subscription's dialog with its current state, down the watcher's flow. `state` is the
  // Subscription-State; a terminated one ends the subscription once it is sent.
  void _notify(Subscription subscription, std::string state);
  void _send_notify(Subscription subscription, std::string state, std::string body, std::function<void()> done);

  // One NOTIFY at a time on a subscription, each reading the record afresh, so CSeq and version follow on.
  using Task = std::function<void(std::function<void()> done)>;
  void _serially(const std::string& id, Task task);
  void _run_next(const std::string& id);

  // The package's document for the presentity, at this subscription's version, read from the datastore. Empty
  // when the state could not be read.
  void _document(const Subscription& subscription, std::function<void(std::optional<std::string>)> then);

  // RFC 4235: the presentity's calls, from its side.
  static std::string _dialog_info(const Subscription& subscription, const std::vector<std::shared_ptr<Call>>& calls);

  void _look_again(const std::string& aor);

  void _schedule(const Subscription& subscription);
  void _expired(const std::string& id);

  void _send_status(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request, std::uint16_t code,
                    const std::string& reason, const std::vector<std::pair<std::string, std::string>>& headers = {});

  static std::string _body(const Subscription& subscription);
  static std::optional<Subscription> _parse(const types::PresenceRecord& record);

  std::shared_ptr<loggers::LoggerScoped> _logger;
  std::weak_ptr<Core> _core;

  // The one thing held in memory: when each subscription this node holds lapses.
  std::map<std::string, std::shared_ptr<Timer>> _timers;

  // Presentities changed in this turn of the strand, looked at once at its end.
  std::set<std::string> _pending;

  // NOTIFYs waiting their turn, by subscription.
  std::map<std::string, std::deque<Task>> _queues;
};

}  // namespace athenasip
