//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <chrono>
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
  static const char* packages() { return "presence, dialog"; }

  // The package the node composes published state for (RFC 3903), as a 489 to PUBLISH lists it.
  static const char* published_packages() { return "presence"; }

  // The Request-URI parameter on the Contact the node gives a watcher, naming the subscription. It brings a
  // refresh to the notifier from any node that serves the realm.
  static constexpr const char* kSubscriptionParameter = "athena-sub";

  // Whether an authorised out-of-dialog request is the notifier's rather than the proxy's: a SUBSCRIBE for a package
  // served here, or any PUBLISH, addressed to an address of record (no Route left, no flow token), not forwarded by a
  // peer, with presence on and a datastore that holds it.
  bool wants(const SIPMessage& request) const;

  // A request `wants` took. `otherwise` routes it as before, when the Request-URI names no realm served here.
  void on_request(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, std::function<void()> otherwise);

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

    // The state the last NOTIFY carried, as _digest has it: what the reconcile compares.
    std::string digest;
  };

  struct Expiry {
    std::uint32_t granted = 0;
    std::uint16_t code = 0;  // 0, or the answer that refuses it
  };

  Expiry _expiry(const SIPMessage& request, bool initial) const;

  // RFC 3903 6: the presentity publishing its own state, as an event state compositor. `previous` is the
  // publication its SIP-If-Match named, if it named one.
  void _publish(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, std::string presentity);
  void _published(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, std::string presentity,
                  std::optional<types::PresenceRecord> previous, std::uint32_t expires);

  // Tells every node that a presentity's state is not what it was, once the change is stored.
  void _signal(const std::string& aor, const std::string& why);

  void _accept(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, std::string presentity, std::string package,
               std::uint32_t expires);
  void _refresh(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, Subscription subscription);

  // Writes the record, then runs `then`: what is answered or sent follows from what is stored.
  void _store(const Subscription& subscription, std::function<void(bool)> then);
  void _end(const Subscription& subscription);

  // A NOTIFY in the subscription's dialog with its current state, down the watcher's flow. `state` is the
  // Subscription-State; a terminated one ends the subscription once it is sent. A `change` of a presentity's
  // presence waits for the subscription's five-second window (RFC 3856 6.6.2); one answering a SUBSCRIBE does not.
  void _notify(Subscription subscription, std::string state, bool change = false);
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

  // RFC 3856: what the presentity published, composed, or else what its registrations say.
  void _presence(const Subscription& subscription, std::function<void(std::optional<std::string>)> then);

  // The package's body type, and its document with nothing in it.
  static std::string _content_type(const std::string& package);
  static std::string _empty_document(const Subscription& subscription);

  void _look_again(const std::string& aor);

  // Every `presence.reconcile` seconds: each subscription this node holds is read back, its expiry timer armed if
  // this node has none (a restart), and notified if its presentity's state is not what it was last sent.
  void _reconcile_schedule();
  void _reconcile();

  // A document's state without what changes on every NOTIFY (dialog-info's version).
  static std::string _digest(const std::string& package, const std::string& document);

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
  std::shared_ptr<Timer> _reconcile_timer;

  // Presentities changed in this turn of the strand, looked at once at its end.
  std::set<std::string> _pending;

  // NOTIFYs waiting their turn, by subscription.
  std::map<std::string, std::deque<Task>> _queues;

  // RFC 3856 6.6.2: when each presence subscription held here was last notified, and the change waiting for its
  // window to open, if one is.
  struct Window {
    std::chrono::steady_clock::time_point last;
    std::shared_ptr<Timer> waiting;
  };
  std::map<std::string, Window> _windows;
};

}  // namespace athenasip
