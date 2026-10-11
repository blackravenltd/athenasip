//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "notifier.h"

#include <algorithm>
#include <boost/json.hpp>
#include <ctime>
#include <utility>

#include "channel.h"
#include "core.h"
#include "events/topics.h"
#include "headers/sip_identity_header.h"
#include "headers/string_header.h"
#include "pidf.h"
#include "util.h"

namespace athenasip {

namespace {

// The default duration of both packages (RFC 4235 3.3, RFC 3856 6.4), and how long a record outlives its expiry so
// the final NOTIFY can still read it.
constexpr std::uint32_t kDefaultExpires = 3600;
constexpr std::uint32_t kGraceSeconds = 32;

// RFC 3856 6.6.2: a presentity's notifications no more often than this.
constexpr std::chrono::seconds kPresenceWindow(5);

std::string value_of(const SIPMessage& message, const std::string& name) {
  return message.header->contains(name) ? message.header->headers_map[name][0]->to_string() : "";
}

// A header that may come as several values, as one comma-separated list.
std::string list_of(const SIPMessage& message, const std::string& name) {
  if (!message.header->contains(name)) return "";
  std::string out;
  for (const auto& value : message.header->headers_map[name]) out += (out.empty() ? "" : ",") + value->to_string();
  return out;
}

// The same Contact, as far as reaching it goes: user, host and port.
bool same_contact(const types::SIPUri& a, const types::SIPUri& b) {
  return a.user == b.user && Util::to_lower(a.host) == Util::to_lower(b.host) && a.port.value_or(0) == b.port.value_or(0);
}

bool serves(const std::string& package) { return package == "presence" || package == "dialog"; }

// RFC 3261 20.1: no Accept means the package's default, which is the type here; otherwise a range must take it.
bool accepts(const SIPMessage& request, const std::string& type) {
  const auto accept = list_of(request, "Accept");
  if (Util::trim(accept).empty()) return true;

  const auto slash = type.find('/');
  std::size_t at = 0;
  while (at <= accept.size()) {
    const auto next = accept.find(',', at);
    const auto range = Util::to_lower(Util::trim(accept.substr(at, next == std::string::npos ? std::string::npos : next - at)));
    const auto media = Util::trim(range.substr(0, range.find(';')));
    if (media == type || media == "*/*" || media == type.substr(0, slash) + "/*") return true;
    if (next == std::string::npos) break;
    at = next + 1;
  }
  return false;
}

std::shared_ptr<types::SIPIdentity> identity_of(const SIPMessage& message, const std::string& name) {
  if (!message.header->contains(name)) return nullptr;
  auto header = message.header->headers_map[name][0]->as<headers::SIPIdentityHeader>();
  return header != nullptr ? header->value : nullptr;
}

std::string tag_of(const SIPMessage& message, const std::string& name) {
  auto identity = identity_of(message, name);
  if (!identity) return "";
  auto found = identity->tags.find("tag");
  return found == identity->tags.end() ? "" : found->second;
}

// RFC 6665 8.2.1: Event = event-type *( SEMI event-param ), compared case-insensitively.
std::string package_of(const std::string& event) { return Util::to_lower(Util::trim(event.substr(0, event.find(';')))); }

std::string event_id_of(const std::string& event) {
  std::size_t at = event.find(';');
  while (at != std::string::npos) {
    const auto next = event.find(';', at + 1);
    const auto param = Util::trim(event.substr(at + 1, next == std::string::npos ? std::string::npos : next - at - 1));
    if (Util::to_lower(param.substr(0, 3)) == "id=") return param.substr(3);
    at = next;
  }
  return "";
}

std::uint64_t cseq_of(const SIPMessage& message) {
  try {
    return std::stoull(value_of(message, "CSeq"));
  } catch (const std::exception&) {
    return 0;
  }
}

std::string xml_escape(const std::string& text) {
  std::string out;
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        out += c;
    }
  }
  return out;
}

}  // namespace

Notifier::Notifier(std::shared_ptr<loggers::Logger> logger, std::weak_ptr<Core> core)
    : _logger(std::make_shared<loggers::LoggerScoped>("notifier", std::move(logger))), _core(std::move(core)) {}

void Notifier::start() {
  auto core = _core.lock();
  if (!core || !core->events) return;

  std::weak_ptr<Notifier> weak_self = weak_from_this();
  core->events->subscribe(
      core->strand(), events::topics::presence_changed_filter(),
      [weak_self](std::string topic, std::string) {
        auto self = weak_self.lock();
        if (!self) return;
        if (const auto aor = events::topics::presence_changed_aor(topic); !aor.empty()) self->changed(aor);
      },
      [this, self = shared_from_this()](plugins::Result<std::shared_ptr<events::Subscription>> subscribed) {
        if (!subscribed.ok) _logger->warn("Cannot listen for presence changes - " + subscribed.error);
      });

  _started_at = core->timer_source()->now();
  _reconcile_schedule();
}

void Notifier::_reconcile_schedule() {
  auto core = _core.lock();
  if (!core || core->config->presence_reconcile == 0) return;

  std::weak_ptr<Notifier> weak_self = weak_from_this();
  _reconcile_timer = core->timer_source()->schedule(std::chrono::seconds(core->config->presence_reconcile), [weak_self]() {
    auto self = weak_self.lock();
    if (!self) return;
    if (auto core = self->_core.lock()) {
      core->post([self]() {
        self->_reconcile();
        self->_reconcile_schedule();
      });
    }
  });
}

void Notifier::_reconcile() {
  auto core = _core.lock();
  if (!core || !core->config->presence_enable || !core->datastore || !core->datastore->supports_presence()) return;

  auto self = shared_from_this();
  core->datastore->presence_list_node(core->strand(), core->config->sip_node_id, [this, self](plugins::Result<std::vector<types::PresenceRecord>> held) {
    if (!held.ok) {
      _logger->error("Cannot read the subscriptions this node holds - " + held.error);
      return;
    }

    for (const auto& record : held.value) {
      if (record.kind != types::PresenceRecord::Kind::Subscription) continue;
      auto subscription = _parse(record);
      if (!subscription) continue;

      // Held here before this node restarted: it lapses as it would have.
      if (!_timers.contains(subscription->id)) _schedule(*subscription);

      _document(*subscription, [this, self, subscription = *subscription](std::optional<std::string> document) {
        if (!document || _digest(subscription.package, *document) == subscription.digest) return;
        _notify(subscription, "active", true);
      });
    }
  });

  _take_over();
}

void Notifier::_take_over() {
  auto core = _core.lock();
  if (!core || !_started_at || !core->nodes()) return;

  // As the orphan sweep judges it: a node that has just started has heard nobody.
  const auto interval = core->config->events_status_interval;
  if (interval == 0) return;
  const auto watch = std::chrono::seconds(interval * 3);
  if (core->timer_source()->now() - *_started_at < watch) return;

  auto self = shared_from_this();
  for (const auto& node : core->nodes()->list(watch)) {
    if (node.id == core->config->sip_node_id || (!node.stale && node.status == "ok")) continue;

    core->datastore->presence_list_node(core->strand(), node.id, [this, self](plugins::Result<std::vector<types::PresenceRecord>> held) {
      if (!held.ok) return;
      for (const auto& record : held.value) {
        if (record.kind != types::PresenceRecord::Kind::Subscription) continue;
        if (auto subscription = _parse(record)) _adopt(std::move(*subscription));
      }
    });
  }
}

void Notifier::_adopt(Subscription subscription) {
  auto self = shared_from_this();
  _watcher_flow(subscription, [this, self, subscription](std::string flow_id) mutable {
    auto core = _core.lock();
    if (!core || flow_id.empty()) return;

    // One node takes it, however many have a flow to the watcher.
    core->datastore->lease(
        core->strand(), "presence-takeover:" + subscription.id, core->config->sip_node_id, 60,
        [this, self, subscription, flow_id](plugins::Result<bool> held) mutable {
          auto core = _core.lock();
          if (!core || !held.ok || !held.value) return;

          const auto from = subscription.node_id;
          subscription.node_id = core->config->sip_node_id;
          subscription.flow_id = flow_id;
          _store(subscription, [this, self, subscription, from](bool stored) {
            if (!stored) return;
            _logger->info("Took over " + subscription.watcher_uri + "'s subscription to " + subscription.presentity + " from " + from + ", which has gone");
            _schedule(subscription);
            _notify(subscription, "active");
          });
        });
  });
}

void Notifier::_watcher_flow(const Subscription& subscription, std::function<void(std::string)> then) {
  auto core = _core.lock();
  if (!core) return;

  auto self = shared_from_this();
  const types::SIPUri target(subscription.remote_target);
  core->subscriber_get(std::make_shared<types::SIPIdentity>(subscription.watcher_uri),
                       [this, self, target, then](plugins::Result<std::shared_ptr<types::Subscriber>> found) {
                         auto core = _core.lock();
                         if (!core) return;
                         if (!found.ok || !found.value || !target.valid) return then("");

                         core->location_list(found.value->id, [this, self, target, then](plugins::Result<std::vector<types::Location>> bindings) {
                           auto core = _core.lock();
                           if (!core) return;
                           if (!bindings.ok) return then("");

                           for (const auto& binding : bindings.value) {
                             if (binding.node_id != core->config->sip_node_id || !binding.contact || !same_contact(*binding.contact, target)) continue;
                             if (auto channel = core->channel_find(binding.flow_id); channel && channel->_connection) return then(binding.flow_id);
                           }
                           then("");
                         });
                       });
}

std::string Notifier::_digest(const std::string& package, const std::string& document) {
  auto state = document;
  if (package == "dialog") {
    // The root's, not the XML declaration's.
    if (const auto at = state.find(" version=\"", state.find("<dialog-info")); at != std::string::npos) {
      if (const auto end = state.find('"', at + 10); end != std::string::npos) state.erase(at, end + 1 - at);
    }
  }
  return Util::md5(state);
}

void Notifier::changed(const std::string& aor) {
  auto core = _core.lock();
  if (!core || !_pending.insert(aor).second) return;

  auto self = shared_from_this();
  core->post([self, aor]() {
    self->_pending.erase(aor);
    self->_look_again(aor);
  });
}

void Notifier::_look_again(const std::string& aor) {
  auto core = _core.lock();
  if (!core) return;

  auto self = shared_from_this();
  core->datastore->presence_list(core->strand(), aor, types::PresenceRecord::Kind::Subscription,
                                 [this, self, aor](plugins::Result<std::vector<types::PresenceRecord>> found) {
                                   auto core = _core.lock();
                                   if (!core) return;
                                   if (!found.ok) {
                                     _logger->error("Cannot read the watchers of " + aor + " - " + found.error);
                                     return;
                                   }

                                   // Each holder sends its own watchers' NOTIFYs, down their flows.
                                   for (const auto& record : found.value) {
                                     if (record.node_id != core->config->sip_node_id) continue;
                                     if (auto subscription = _parse(record)) _notify(std::move(*subscription), "active", true);
                                   }
                                 });
}

bool Notifier::wants(const SIPMessage& request) const {
  auto core = _core.lock();
  if (!core || !core->config->presence_enable || !core->datastore || !core->datastore->supports_presence()) return false;

  // RFC 3903: a PUBLISH is never in a dialog, and one for a package not composed here is answered 489.
  const auto& method = request.header->request_method;
  if (method != "PUBLISH") {
    if (method != "SUBSCRIBE" || !tag_of(request, "To").empty()) return false;
    if (!serves(package_of(value_of(request, "Event")))) return false;
  }

  // Passing through, or routed back down a flow: not addressed to the AOR.
  if (request.header->contains("Route") || !request.flow_token.empty()) return false;

  // A peer that forwarded it chose not to answer it, and is the one that knows the watcher.
  if (auto channel = request.channel.lock(); channel && !channel->peer_node().empty()) return false;

  return request.header->request_uri && !request.header->request_uri->user.empty();
}

void Notifier::on_request(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, std::function<void()> otherwise) {
  auto core = _core.lock();
  if (!core) return;

  auto self = shared_from_this();
  const auto host = Util::to_lower(request->header->request_uri->host);

  core->realm_get_by_name(host, [this, self, request, transaction, otherwise](plugins::Result<std::shared_ptr<types::Realm>> found) {
    auto core = _core.lock();
    if (!core) return;

    if (!found.ok) {
      _logger->error("Cannot read a realm for a " + request->header->request_method + " - " + found.error);
      return _send_status(transaction, request, 500, "Server Internal Error");
    }
    if (!found.value) return otherwise();

    const auto realm = found.value;

    if (request->header->request_method == "PUBLISH") return _publish(request, transaction, "sip:" + request->header->request_uri->user + "@" + realm->name);

    // RFC 6665 4.1.2.1: the Contact is the dialog's remote target.
    auto contact = identity_of(*request, "Contact");
    if (!contact || !contact->uri || !contact->uri->valid) {
      _logger->info("SUBSCRIBE with no usable Contact - 400");
      return _send_status(transaction, request, 400, "Bad Request");
    }

    const auto package = package_of(value_of(*request, "Event"));

    // RFC 6665 4.2.1.1: a watcher that cannot read the package's document.
    if (!accepts(*request, _content_type(package))) {
      _logger->info("SUBSCRIBE for " + package + " that does not accept " + _content_type(package) + " - 406");
      return _send_status(transaction, request, 406, "Not Acceptable", {{"Accept", _content_type(package)}});
    }

    const auto expiry = _expiry(*request, true);
    if (expiry.code == 423) {
      return _send_status(transaction, request, 423, "Interval Too Brief", {{"Min-Expires", std::to_string(core->config->presence_min_expires)}});
    }

    const auto presentity = "sip:" + request->header->request_uri->user + "@" + realm->name;

    auto identity = std::make_shared<types::SIPIdentity>(presentity);
    core->subscriber_get(
        identity, [this, self, request, transaction, realm, presentity, package, expiry](plugins::Result<std::shared_ptr<types::Subscriber>> found) {
          if (!found.ok) {
            _logger->error("Cannot read " + presentity + " for a SUBSCRIBE - " + found.error);
            return _send_status(transaction, request, 500, "Server Internal Error");
          }
          if (!found.value) {
            _logger->info("SUBSCRIBE to " + presentity + ", who is nobody here - 404");
            return _send_status(transaction, request, 404, "Not Found");
          }

          // Decided 2026-10-11: a watcher is a subscriber proven to be one, in the presentity's own realm.
          const auto& watcher = request->authenticated_aor;
          const auto at = watcher.rfind('@');
          if (watcher.empty() || at == std::string::npos || watcher.substr(at + 1) != realm->name) {
            _logger->info("SUBSCRIBE to " + presentity + " from " + (watcher.empty() ? std::string("an unproven watcher") : watcher) + " - 403");
            return _send_status(transaction, request, 403, "Forbidden");
          }

          _accept(request, transaction, presentity, package, expiry.granted);
        });
  });
}

bool Notifier::owns(const SIPMessage& request) {
  return request.header->request_uri && request.header->request_uri->has_parameter(kSubscriptionParameter) && !tag_of(request, "To").empty();
}

void Notifier::on_in_dialog(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction) {
  auto core = _core.lock();
  if (!core) return;

  // Only the watcher sends in this dialog, and only to refresh or end it (RFC 6665 4.1.2.2).
  if (request->header->request_method == "ACK") return;
  if (request->header->request_method != "SUBSCRIBE") {
    return _send_status(transaction, request, 405, "Method Not Allowed", {{"Allow", "SUBSCRIBE"}});
  }

  const auto id = request->header->request_uri->parameter(kSubscriptionParameter);
  auto self = shared_from_this();

  core->datastore->presence_get(core->strand(), types::PresenceRecord::Kind::Subscription, id,
                                [this, self, request, transaction, id](plugins::Result<std::optional<types::PresenceRecord>> found) {
                                  if (!found.ok) {
                                    _logger->error("Cannot read subscription " + id + " - " + found.error);
                                    return _send_status(transaction, request, 500, "Server Internal Error");
                                  }

                                  auto subscription = found.value ? _parse(*found.value) : std::nullopt;

                                  // RFC 3261 12.2.2: the dialog is the Call-ID and both tags.
                                  if (!subscription || subscription->call_id != value_of(*request, "Call-ID") ||
                                      subscription->remote_tag != tag_of(*request, "From") || subscription->local_tag != tag_of(*request, "To")) {
                                    _logger->info("SUBSCRIBE in no subscription this node knows - 481");
                                    return _send_status(transaction, request, 481, "Call/Transaction Does Not Exist");
                                  }

                                  // RFC 3261 12.2.2: a CSeq not above the last is out of order.
                                  const auto cseq = cseq_of(*request);
                                  if (cseq <= subscription->remote_cseq) return _send_status(transaction, request, 500, "Server Internal Error");

                                  // RFC 6665 4.5.2: one subscription to a dialog.
                                  const auto event = value_of(*request, "Event");
                                  if (package_of(event) != subscription->package || event_id_of(event) != subscription->event_id) {
                                    return _send_status(transaction, request, 489, "Bad Event", {{"Allow-Events", packages()}});
                                  }

                                  subscription->remote_cseq = cseq;
                                  _refresh(request, transaction, std::move(*subscription));
                                });
}

Notifier::Expiry Notifier::_expiry(const SIPMessage& request, bool initial) const {
  (void)initial;
  auto core = _core.lock();
  Expiry expiry;
  expiry.granted = kDefaultExpires;

  if (request.header->contains("Expires")) {
    try {
      expiry.granted = static_cast<std::uint32_t>(std::stoul(value_of(request, "Expires")));
    } catch (const std::exception&) {
      expiry.granted = kDefaultExpires;
    }
  }

  // Zero is an unsubscribe, or on a new SUBSCRIBE a fetch (RFC 6665 4.1.2.3, 4.2.1.1).
  if (expiry.granted == 0 || !core) return expiry;

  if (expiry.granted < core->config->presence_min_expires) {
    expiry.code = 423;
    return expiry;
  }

  expiry.granted = std::min(expiry.granted, core->config->presence_max_expires);
  return expiry;
}

void Notifier::_publish(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, std::string presentity) {
  auto core = _core.lock();
  if (!core) return;

  auto self = shared_from_this();
  auto identity = std::make_shared<types::SIPIdentity>(presentity);
  core->subscriber_get(identity, [this, self, request, transaction, presentity](plugins::Result<std::shared_ptr<types::Subscriber>> found) {
    auto core = _core.lock();
    if (!core) return;

    // RFC 3903 6 step 1: the presentity is one, and the publisher is it. Fixed, not policy (docs/design/presence.md).
    if (!found.ok) {
      _logger->error("Cannot read " + presentity + " for a PUBLISH - " + found.error);
      return _send_status(transaction, request, 500, "Server Internal Error");
    }
    if (!found.value) {
      _logger->info("PUBLISH for " + presentity + ", who is nobody here - 404");
      return _send_status(transaction, request, 404, "Not Found");
    }
    if (request->authenticated_aor != presentity) {
      const auto& publisher = request->authenticated_aor;
      _logger->info("PUBLISH for " + presentity + " from " + (publisher.empty() ? std::string("an unproven publisher") : publisher) + " - 403");
      return _send_status(transaction, request, 403, "Forbidden");
    }

    // Step 2.
    if (package_of(value_of(*request, "Event")) != published_packages()) {
      return _send_status(transaction, request, 489, "Bad Event", {{"Allow-Events", published_packages()}});
    }

    // Step 3.
    const auto expiry = _expiry(*request, true);
    if (expiry.code == 423) {
      return _send_status(transaction, request, 423, "Interval Too Brief", {{"Min-Expires", std::to_string(core->config->presence_min_expires)}});
    }

    // Step 4: an entity tag names the publication this one refreshes, modifies or removes; without one it is new,
    // and must carry state.
    const auto if_match = Util::trim(value_of(*request, "SIP-If-Match"));
    if (if_match.empty()) {
      if (request->body.empty()) return _send_status(transaction, request, 400, "Bad Request");
      return _published(request, transaction, presentity, std::nullopt, expiry.granted);
    }

    core->datastore->presence_get(
        core->strand(), types::PresenceRecord::Kind::Publication, if_match,
        [this, self, request, transaction, presentity, if_match, expiry](plugins::Result<std::optional<types::PresenceRecord>> found) {
          if (!found.ok) {
            _logger->error("Cannot read publication " + if_match + " - " + found.error);
            return _send_status(transaction, request, 500, "Server Internal Error");
          }
          if (!found.value || found.value->aor != presentity) {
            _logger->info("PUBLISH for " + presentity + " naming a publication it does not have - 412");
            return _send_status(transaction, request, 412, "Conditional Request Failed");
          }
          _published(request, transaction, presentity, std::move(found.value), expiry.granted);
        });
  });
}

void Notifier::_published(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, std::string presentity,
                          std::optional<types::PresenceRecord> previous, std::uint32_t expires) {
  auto core = _core.lock();
  if (!core) return;

  const bool removal = previous && expires == 0;
  const bool has_body = !request->body.empty();

  // Step 5, RFC 3856 6.1: presence state is PIDF. A removal's body is not read.
  if (has_body && !removal) {
    const auto content_type = value_of(*request, "Content-Type");
    if (Util::to_lower(Util::trim(content_type.substr(0, content_type.find(';')))) != "application/pidf+xml") {
      return _send_status(transaction, request, 415, "Unsupported Media Type", {{"Accept", "application/pidf+xml"}});
    }
  }

  // Step 6: a new entity tag on every success, 128 random bits or more.
  const auto etag = Util::generate_random_string("", 24);
  const bool changed = removal || (has_body && expires > 0);

  auto self = shared_from_this();
  auto answer = [this, self, request, transaction, presentity, etag, expires, changed]() {
    auto response = request->generate_response();
    response->header->response_code = 200;
    response->header->response_message = "OK";
    response->header->add("SIP-ETag", std::make_shared<headers::StringHeader>(etag));
    response->header->add("Expires", std::make_shared<headers::StringHeader>(std::to_string(expires)));
    transaction->send(response);
    if (changed) _signal(presentity, "{\"publication\":\"" + etag + "\"}");
  };

  // The tag a refresh, modification or removal named is spent once the new state is stored.
  auto retire = [this, self, previous, answer]() {
    auto core = _core.lock();
    if (!core) return;
    if (!previous) return answer();

    core->datastore->presence_delete(core->strand(), types::PresenceRecord::Kind::Publication, previous->id,
                                     [this, self, id = previous->id, answer](plugins::Status status) {
                                       if (!status.ok) _logger->error("Cannot remove publication " + id + " - " + status.error);
                                       answer();
                                     });
  };

  if (removal) {
    _logger->info(presentity + " withdraws publication " + previous->id);
    return retire();
  }

  // RFC 3903 4.1 asks nothing of an initial publication for no time at all; there is nothing to keep.
  if (expires == 0) return answer();

  types::PresenceRecord record;
  record.kind = types::PresenceRecord::Kind::Publication;
  record.id = etag;
  record.aor = presentity;
  record.content_type = "application/pidf+xml";
  record.body = has_body ? request->body : previous->body;

  core->datastore->presence_put(core->strand(), std::move(record), expires,
                                [this, self, request, transaction, presentity, etag, expires, retire](plugins::Status status) {
                                  if (!status.ok) {
                                    _logger->error("Cannot store a publication for " + presentity + " - " + status.error);
                                    return _send_status(transaction, request, 500, "Server Internal Error");
                                  }
                                  _logger->info(presentity + " publishes " + etag + " for " + std::to_string(expires) + "s");
                                  retire();
                                });
}

void Notifier::_signal(const std::string& aor, const std::string& why) {
  auto core = _core.lock();
  if (!core || !core->events) return;
  core->events->publish(events::topics::presence_changed(aor), why);
}

void Notifier::_accept(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, std::string presentity,
                       std::string package, std::uint32_t expires) {
  auto core = _core.lock();
  if (!core) return;

  auto channel = request->channel.lock();
  const auto realm = presentity.substr(presentity.rfind('@') + 1);
  const auto event = value_of(*request, "Event");

  auto response = request->generate_response();

  Subscription subscription;
  subscription.id = Util::generate_random_string("", 24);
  subscription.presentity = presentity;
  subscription.package = std::move(package);
  subscription.event_id = event_id_of(event);
  subscription.call_id = value_of(*request, "Call-ID");
  subscription.local_tag = tag_of(*response, "To");
  subscription.remote_tag = tag_of(*request, "From");
  subscription.watcher_uri = identity_of(*request, "From")->uri->to_string();
  subscription.presentity_uri = identity_of(*request, "To") && identity_of(*request, "To")->uri ? identity_of(*request, "To")->uri->to_string() : presentity;
  subscription.remote_target = identity_of(*request, "Contact")->uri->to_string();
  if (request->header->contains("Record-Route")) {
    for (const auto& route : request->header->headers_map["Record-Route"]) subscription.route_set.push_back(route->to_string());
  }

  // In the presentity's realm, not at this node: the watcher reached this node through the realm, and a refresh
  // that does the same reaches whichever node serves it, which reads the subscription back by this parameter.
  const auto transport = channel && channel->_connection ? Util::to_lower(channel->_connection->transport_name()) : std::string("udp");
  subscription.contact = "sip:" + realm + (transport == "udp" ? "" : ";transport=" + transport) + ";" + kSubscriptionParameter + "=" + subscription.id;

  subscription.remote_cseq = cseq_of(*request);
  subscription.expires = expires;
  subscription.expires_at = std::time(nullptr) + static_cast<std::time_t>(expires);
  subscription.flow_id = channel ? channel->flow_id() : "";
  subscription.node_id = core->config->sip_node_id;

  response->header->response_code = 200;
  response->header->response_message = "OK";
  response->header->add("Expires", std::make_shared<headers::StringHeader>(std::to_string(expires)));
  response->header->add("Contact", std::make_shared<headers::StringHeader>("<" + subscription.contact + ">"));
  response->header->add("Allow-Events", std::make_shared<headers::StringHeader>(packages()));

  // RFC 6665 4.2.1.1: Expires 0 asks for the state once. Nothing is kept.
  if (expires == 0) {
    transaction->send(response);
    return _notify(std::move(subscription), "terminated;reason=timeout");
  }

  auto self = shared_from_this();
  _store(subscription, [this, self, request, transaction, response, subscription](bool stored) {
    if (!stored) return _send_status(transaction, request, 500, "Server Internal Error");

    _logger->info(subscription.watcher_uri + " watches " + subscription.presentity + " (" + subscription.package + ") for " +
                  std::to_string(subscription.expires) + "s");
    transaction->send(response);
    _schedule(subscription);
    _notify(subscription, "active");
  });
}

void Notifier::_refresh(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction, Subscription subscription) {
  auto core = _core.lock();
  if (!core) return;

  const auto expiry = _expiry(*request, false);
  if (expiry.code == 423) {
    return _send_status(transaction, request, 423, "Interval Too Brief", {{"Min-Expires", std::to_string(core->config->presence_min_expires)}});
  }

  // RFC 6665 4.1.2.2: SUBSCRIBE refreshes the target.
  if (auto contact = identity_of(*request, "Contact"); contact && contact->uri && contact->uri->valid) subscription.remote_target = contact->uri->to_string();

  // The watcher's flow is the one its latest SUBSCRIBE came over, and the node that has it holds the subscription.
  if (auto channel = request->channel.lock()) subscription.flow_id = channel->flow_id();
  subscription.node_id = core->config->sip_node_id;

  auto response = request->generate_response();
  response->header->response_code = 200;
  response->header->response_message = "OK";
  response->header->add("Expires", std::make_shared<headers::StringHeader>(std::to_string(expiry.granted)));
  response->header->add("Contact", std::make_shared<headers::StringHeader>("<" + subscription.contact + ">"));
  response->header->add("Allow-Events", std::make_shared<headers::StringHeader>(packages()));

  // RFC 6665 4.1.2.3: Expires 0 ends it, with a final NOTIFY.
  if (expiry.granted == 0) {
    _logger->info(subscription.watcher_uri + " no longer watches " + subscription.presentity);
    transaction->send(response);
    return _notify(std::move(subscription), "terminated;reason=timeout");
  }

  subscription.expires = expiry.granted;
  subscription.expires_at = std::time(nullptr) + static_cast<std::time_t>(expiry.granted);

  auto self = shared_from_this();
  _store(subscription, [this, self, request, transaction, response, subscription](bool stored) {
    if (!stored) return _send_status(transaction, request, 500, "Server Internal Error");

    transaction->send(response);
    _schedule(subscription);
    _notify(subscription, "active");
  });
}

void Notifier::_store(const Subscription& subscription, std::function<void(bool)> then) {
  auto core = _core.lock();
  if (!core) return;

  types::PresenceRecord record;
  record.kind = types::PresenceRecord::Kind::Subscription;
  record.id = subscription.id;
  record.aor = subscription.presentity;
  record.node_id = subscription.node_id;
  record.content_type = "application/json";
  record.body = _body(subscription);

  const auto remaining = subscription.expires_at - std::time(nullptr);
  const auto seconds = static_cast<std::uint32_t>(std::max<std::time_t>(remaining, 1)) + kGraceSeconds;

  auto self = shared_from_this();
  core->datastore->presence_put(core->strand(), std::move(record), seconds, [this, self, id = subscription.id, then](plugins::Status status) {
    if (!status.ok) _logger->error("Cannot store subscription " + id + " - " + status.error);
    then(status.ok);
  });
}

void Notifier::_end(const Subscription& subscription) {
  auto core = _core.lock();
  if (!core) return;

  if (auto timer = _timers.find(subscription.id); timer != _timers.end()) {
    if (timer->second) timer->second->cancel();
    _timers.erase(timer);
  }
  if (auto window = _windows.find(subscription.id); window != _windows.end()) {
    if (window->second.waiting) window->second.waiting->cancel();
    _windows.erase(window);
  }

  auto self = shared_from_this();
  core->datastore->presence_delete(core->strand(), types::PresenceRecord::Kind::Subscription, subscription.id,
                                   [this, self, id = subscription.id](plugins::Status status) {
                                     if (!status.ok) _logger->error("Cannot remove subscription " + id + " - " + status.error);
                                   });
}

void Notifier::_notify(Subscription subscription, std::string state, bool change) {
  const auto id = subscription.id;
  auto self = shared_from_this();

  // RFC 3856 6.6.2: a change inside the window waits for it to open, and then sends the state as it is by then.
  if (change && subscription.package == "presence") {
    auto core = _core.lock();
    if (!core) return;

    const auto now = core->timer_source()->now();
    if (auto window = _windows.find(id); window != _windows.end() && now - window->second.last < kPresenceWindow) {
      if (window->second.waiting) return;

      std::weak_ptr<Notifier> weak_self = weak_from_this();
      const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(window->second.last + kPresenceWindow - now);
      window->second.waiting = core->timer_source()->schedule(wait, [weak_self, subscription]() {
        auto self = weak_self.lock();
        if (!self) return;
        if (auto core = self->_core.lock()) {
          core->post([self, subscription]() {
            if (auto window = self->_windows.find(subscription.id); window != self->_windows.end()) window->second.waiting.reset();
            self->_notify(subscription, "active", true);
          });
        }
      });
      return;
    }
  }

  _serially(id, [this, self, subscription = std::move(subscription), state = std::move(state)](std::function<void()> done) mutable {
    auto core = _core.lock();
    if (!core) return done();

    const bool terminated = state.rfind("terminated", 0) == 0;

    core->datastore->presence_get(core->strand(), types::PresenceRecord::Kind::Subscription, subscription.id,
                                  [this, self, subscription, state, terminated, done](plugins::Result<std::optional<types::PresenceRecord>> found) mutable {
                                    if (!found.ok) {
                                      _logger->error("Cannot read subscription " + subscription.id + " - " + found.error);
                                      return done();
                                    }

                                    // The record is the truth: the last NOTIFY's CSeq and version, and the expiry as last refreshed. A fetch
                                    // has none, and is its own last word.
                                    if (auto current = found.value ? _parse(*found.value) : std::nullopt) {
                                      subscription = std::move(*current);
                                    } else if (!terminated) {
                                      return done();
                                    }

                                    if (!terminated) {
                                      const auto remaining = std::max<std::time_t>(subscription.expires_at - std::time(nullptr), 0);
                                      state += ";expires=" + std::to_string(remaining);
                                    }

                                    _document(subscription, [this, self, subscription, state, terminated, done](std::optional<std::string> body) mutable {
                                      // A state that cannot be read is not reported as empty, except in the last word on a subscription.
                                      if (!body && !terminated) return done();
                                      auto document = body ? std::move(*body) : _empty_document(subscription);
                                      _send_notify(std::move(subscription), std::move(state), std::move(document), done);
                                    });
                                  });
  });
}

void Notifier::_serially(const std::string& id, Task task) {
  auto& queue = _queues[id];
  queue.push_back(std::move(task));
  if (queue.size() == 1) _run_next(id);
}

void Notifier::_run_next(const std::string& id) {
  auto queue = _queues.find(id);
  if (queue == _queues.end() || queue->second.empty()) return;

  auto task = queue->second.front();
  std::weak_ptr<Notifier> weak_self = weak_from_this();
  task([weak_self, id]() {
    auto self = weak_self.lock();
    if (!self) return;
    auto queue = self->_queues.find(id);
    if (queue == self->_queues.end()) return;
    queue->second.pop_front();
    if (queue->second.empty()) {
      self->_queues.erase(queue);
    } else {
      self->_run_next(id);
    }
  });
}

void Notifier::_send_notify(Subscription subscription, std::string state, std::string body, std::function<void()> done) {
  auto self = shared_from_this();
  _reach(subscription, [this, self, subscription, state, body, done](std::shared_ptr<Channel> channel) mutable {
    if (!channel) {
      _logger->info("There is no way to " + subscription.watcher_uri + " any more; ending its subscription to " + subscription.presentity);
      _end(subscription);
      return done();
    }
    _send_notify_on(std::move(subscription), std::move(state), std::move(body), std::move(done), std::move(channel));
  });
}

void Notifier::_reach(const Subscription& subscription, std::function<void(std::shared_ptr<Channel>)> then) {
  auto core = _core.lock();
  if (!core) return;

  if (auto channel = core->channel_find(subscription.flow_id); channel && channel->_connection) return then(channel);

  auto self = shared_from_this();
  _watcher_flow(subscription, [this, self, subscription, then](std::string flow_id) {
    auto core = _core.lock();
    if (!core) return;
    if (auto channel = flow_id.empty() ? nullptr : core->channel_find(flow_id); channel && channel->_connection) return then(channel);

    // RFC 3261 12.2.1.1: to the first Route when there is a route set, else to the remote target.
    auto next = subscription.remote_target;
    if (!subscription.route_set.empty()) {
      const auto& route = subscription.route_set.front();
      const auto open = route.find('<');
      next = open == std::string::npos ? route : route.substr(open + 1, route.find('>', open) - open - 1);
    }
    const types::SIPUri uri(next);
    if (!uri.valid || uri.host.empty()) return then(nullptr);

    core->locator()->locate(core->strand(), uri, [this, self, then](plugins::Result<std::vector<dns::Hop>> located) {
      if (!located.ok || located.value.empty()) return then(nullptr);
      _connect(std::move(located.value), 0, then);
    });
  });
}

void Notifier::_connect(std::vector<dns::Hop> hops, std::size_t index, std::function<void(std::shared_ptr<Channel>)> then) {
  auto core = _core.lock();
  if (!core) return;
  if (index >= hops.size()) return then(nullptr);

  const auto hop = hops[index];
  auto self = shared_from_this();
  core->channel_connect(hop.transport, hop.address, hop.port,
                        [this, self, hops = std::move(hops), index, then](plugins::Result<std::shared_ptr<Channel>> opened) mutable {
                          if (opened.ok && opened.value && opened.value->_connection) return then(opened.value);
                          _connect(std::move(hops), index + 1, then);
                        });
}

void Notifier::_send_notify_on(Subscription subscription, std::string state, std::string body, std::function<void()> done, std::shared_ptr<Channel> channel) {
  auto core = _core.lock();
  if (!core) return done();

  const bool terminated = state.rfind("terminated", 0) == 0;

  // The flow it now goes down is the one to use next time.
  subscription.flow_id = channel->flow_id();

  subscription.local_cseq += 1;
  subscription.version += 1;

  const auto transport = Util::to_upper(channel->_connection->transport_name());
  const auto advertised = core->advertised_for(*channel);
  const auto branch = std::string("z9hG4bK") + Util::generate_random_string("", 16);

  std::string raw = "NOTIFY " + subscription.remote_target + " SIP/2.0\r\n";
  raw += "Via: SIP/2.0/" + transport + " " + advertised.host + ":" + std::to_string(advertised.port) + ";branch=" + branch + ";rport\r\n";
  raw += "Max-Forwards: 70\r\n";
  for (const auto& route : subscription.route_set) raw += "Route: " + route + "\r\n";
  raw += "From: <" + subscription.presentity_uri + ">;tag=" + subscription.local_tag + "\r\n";
  raw += "To: <" + subscription.watcher_uri + ">;tag=" + subscription.remote_tag + "\r\n";
  raw += "Call-ID: " + subscription.call_id + "\r\n";
  raw += "CSeq: " + std::to_string(subscription.local_cseq) + " NOTIFY\r\n";
  raw += "Contact: <" + subscription.contact + ">\r\n";
  raw += "Event: " + subscription.package + (subscription.event_id.empty() ? "" : ";id=" + subscription.event_id) + "\r\n";
  raw += "Subscription-State: " + state + "\r\n";
  raw += "Allow-Events: " + std::string(packages()) + "\r\n";
  raw += "Content-Type: " + _content_type(subscription.package) + "\r\n";

  auto notify = std::make_shared<SIPMessage>();
  notify->header = std::make_shared<SIPHeader>(raw);
  notify->body = body;
  notify->body_length = static_cast<unsigned int>(body.size());
  notify->branch = branch;

  auto self = shared_from_this();
  auto send = [this, self, notify, channel, subscription]() {
    auto core = _core.lock();
    if (!core) return;

    // RFC 6665 4.2.2: a NOTIFY that fails ends the subscription.
    core->client_transaction_start(
        notify, channel,
        [this, self, subscription](std::shared_ptr<SIPMessage> response) {
          if (!response || !response->header || response->header->response_code < 300) return;
          _logger->info("A NOTIFY to " + subscription.watcher_uri + " was answered " + std::to_string(response->header->response_code) +
                        "; ending its subscription to " + subscription.presentity);
          _end(subscription);
        },
        [this, self, subscription]() {
          _logger->info("A NOTIFY to " + subscription.watcher_uri + " had no answer; ending its subscription to " + subscription.presentity);
          _end(subscription);
        });
  };

  // A final NOTIFY ends the subscription, and a live one is stored before it goes, with its new CSeq and version,
  // so the next NOTIFY from any node follows on from it.
  if (terminated) {
    _end(subscription);
    send();
    return done();
  }

  if (subscription.package == "presence") _windows[subscription.id].last = core->timer_source()->now();
  subscription.digest = _digest(subscription.package, body);

  _store(subscription, [send, done](bool) mutable {
    send();
    done();
  });
}

void Notifier::_document(const Subscription& subscription, std::function<void(std::optional<std::string>)> then) {
  auto core = _core.lock();
  if (!core) return;

  if (subscription.package == "presence") return _presence(subscription, std::move(then));

  auto self = shared_from_this();
  core->datastore->call_list_for_subscriber(core->strand(), subscription.presentity,
                                            [this, self, subscription, then](plugins::Result<std::vector<std::shared_ptr<Call>>> calls) {
                                              if (!calls.ok) {
                                                _logger->error("Cannot read the calls of " + subscription.presentity + " - " + calls.error);
                                                return then(std::nullopt);
                                              }
                                              then(_dialog_info(subscription, calls.value));
                                            });
}

// RFC 4235 4.1: the full state, each of the presentity's dialogs from its side, at this subscription's version.
std::string Notifier::_dialog_info(const Subscription& subscription, const std::vector<std::shared_ptr<Call>>& calls) {
  std::string dialogs;

  for (const auto& call : calls) {
    if (!call) continue;

    for (std::size_t i = 0; i < call->participants.size(); ++i) {
      const auto& leg = call->participants[i];
      if (leg.subscriber != subscription.presentity) continue;

      // RFC 4235 3.7.1, from the presentity's side. A callee not yet reached has no dialog to report.
      std::string state;
      switch (call->state) {
        case Call::State::Initial:
        case Call::State::Trying:
          state = leg.originator ? "trying" : "";
          break;
        case Call::State::Ringing:
          state = "early";
          break;
        case Call::State::Connected:
          state = "confirmed";
          break;
        case Call::State::Closing:
          state = "terminated";
          break;
        case Call::State::Closed:
          break;
      }
      if (state.empty()) continue;

      const std::shared_ptr<types::SIPIdentity> remote = call->participants.size() == 2 ? call->participants[1 - i].identity : nullptr;

      // The tags as the presentity's end sees them: its own is local.
      std::string local_tag;
      std::string remote_tag;
      if (leg.dialog) {
        local_tag = leg.originator ? leg.dialog->caller_tag : leg.dialog->callee_tag;
        remote_tag = leg.originator ? leg.dialog->callee_tag : leg.dialog->caller_tag;
      }

      dialogs += "<dialog id=\"" + xml_escape(call->id + (leg.originator ? "-i" : "-r")) + "\" call-id=\"" + xml_escape(call->id) + "\"";
      if (!local_tag.empty()) dialogs += " local-tag=\"" + xml_escape(local_tag) + "\"";
      if (!remote_tag.empty()) dialogs += " remote-tag=\"" + xml_escape(remote_tag) + "\"";
      dialogs += std::string(" direction=\"") + (leg.originator ? "initiator" : "recipient") + "\">";
      dialogs += "<state>" + state + "</state>";
      if (leg.identity && leg.identity->uri) dialogs += "<local><identity>" + xml_escape(leg.identity->uri->to_string()) + "</identity></local>";
      if (remote && remote->uri) dialogs += "<remote><identity>" + xml_escape(remote->uri->to_string()) + "</identity></remote>";
      dialogs += "</dialog>";
    }
  }

  std::string document = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\" version=\"" +
                         std::to_string(subscription.version) + "\" state=\"full\" entity=\"" + xml_escape(subscription.presentity) + "\"";
  return document + (dialogs.empty() ? "/>\r\n" : ">" + dialogs + "</dialog-info>\r\n");
}

void Notifier::_presence(const Subscription& subscription, std::function<void(std::optional<std::string>)> then) {
  auto core = _core.lock();
  if (!core) return;

  auto self = shared_from_this();
  const auto presentity = subscription.presentity;

  core->datastore->presence_list(
      core->strand(), presentity, types::PresenceRecord::Kind::Publication,
      [this, self, presentity, then](plugins::Result<std::vector<types::PresenceRecord>> published) {
        auto core = _core.lock();
        if (!core) return;
        if (!published.ok) {
          _logger->error("Cannot read what " + presentity + " has published - " + published.error);
          return then(std::nullopt);
        }

        // The later publication's tuple wins a clash. The record holds no time of publishing, so the one that
        // lasts longer stands for it: a device refreshes before it lapses.
        if (!published.value.empty()) {
          auto records = std::move(published.value);
          std::stable_sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.expires_at < b.expires_at; });
          std::vector<std::string> documents;
          for (const auto& record : records) documents.push_back(record.body);
          return then(pidf::compose(presentity, documents));
        }

        // Nothing published: whether it has a binding.
        core->subscriber_get(std::make_shared<types::SIPIdentity>(presentity),
                             [this, self, presentity, then](plugins::Result<std::shared_ptr<types::Subscriber>> found) {
                               auto core = _core.lock();
                               if (!core) return;
                               if (!found.ok) {
                                 _logger->error("Cannot read " + presentity + " - " + found.error);
                                 return then(std::nullopt);
                               }
                               if (!found.value) return then(pidf::registration(presentity, false));

                               core->location_list(found.value->id, [this, self, presentity, then](plugins::Result<std::vector<types::Location>> bindings) {
                                 if (!bindings.ok) {
                                   _logger->error("Cannot read the bindings of " + presentity + " - " + bindings.error);
                                   return then(std::nullopt);
                                 }
                                 then(pidf::registration(presentity, !bindings.value.empty()));
                               });
                             });
      });
}

std::string Notifier::_content_type(const std::string& package) { return package == "presence" ? "application/pidf+xml" : "application/dialog-info+xml"; }

std::string Notifier::_empty_document(const Subscription& subscription) {
  return subscription.package == "presence" ? pidf::registration(subscription.presentity, false) : _dialog_info(subscription, {});
}

void Notifier::_schedule(const Subscription& subscription) {
  auto core = _core.lock();
  if (!core) return;

  if (auto existing = _timers.find(subscription.id); existing != _timers.end() && existing->second) existing->second->cancel();

  std::weak_ptr<Notifier> weak_self = weak_from_this();
  const auto id = subscription.id;
  const auto remaining = std::max<std::time_t>(subscription.expires_at - std::time(nullptr), 1);
  _timers[id] = core->timer_source()->schedule(std::chrono::seconds(remaining), [weak_self, id]() {
    auto self = weak_self.lock();
    if (!self) return;
    if (auto core = self->_core.lock()) core->post([self, id]() { self->_expired(id); });
  });
}

void Notifier::_expired(const std::string& id) {
  auto core = _core.lock();
  if (!core) return;

  _timers.erase(id);

  auto self = shared_from_this();
  core->datastore->presence_get(core->strand(), types::PresenceRecord::Kind::Subscription, id,
                                [this, self, id](plugins::Result<std::optional<types::PresenceRecord>> found) {
                                  auto core = _core.lock();
                                  if (!core || !found.ok || !found.value) return;

                                  auto subscription = _parse(*found.value);

                                  // Refreshed elsewhere, or here since this timer was set: not this timer's to end.
                                  if (!subscription || subscription->node_id != core->config->sip_node_id || _timers.contains(id)) return;

                                  _logger->info(subscription->watcher_uri + "'s subscription to " + subscription->presentity + " has expired");
                                  _notify(std::move(*subscription), "terminated;reason=timeout");
                                });
}

void Notifier::_send_status(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request, std::uint16_t code,
                            const std::string& reason, const std::vector<std::pair<std::string, std::string>>& headers) {
  if (!transaction) return;

  auto response = request->generate_response();
  response->header->response_code = code;
  response->header->response_message = reason;
  for (const auto& [name, value] : headers) response->header->add(name, std::make_shared<headers::StringHeader>(value));
  transaction->send(response);
}

std::string Notifier::_body(const Subscription& subscription) {
  boost::json::object obj;
  obj["id"] = subscription.id;
  obj["presentity"] = subscription.presentity;
  obj["package"] = subscription.package;
  obj["event_id"] = subscription.event_id;
  obj["call_id"] = subscription.call_id;
  obj["local_tag"] = subscription.local_tag;
  obj["remote_tag"] = subscription.remote_tag;
  obj["watcher_uri"] = subscription.watcher_uri;
  obj["presentity_uri"] = subscription.presentity_uri;
  obj["remote_target"] = subscription.remote_target;

  boost::json::array routes;
  for (const auto& route : subscription.route_set) routes.push_back(boost::json::value(route));
  obj["route_set"] = std::move(routes);

  obj["contact"] = subscription.contact;
  obj["local_cseq"] = subscription.local_cseq;
  obj["remote_cseq"] = subscription.remote_cseq;
  obj["version"] = subscription.version;
  obj["expires"] = subscription.expires;
  obj["expires_at"] = static_cast<std::int64_t>(subscription.expires_at);
  obj["flow_id"] = subscription.flow_id;
  obj["node_id"] = subscription.node_id;
  obj["digest"] = subscription.digest;
  return boost::json::serialize(obj);
}

std::optional<Notifier::Subscription> Notifier::_parse(const types::PresenceRecord& record) {
  try {
    const auto parsed = boost::json::parse(record.body);
    const auto& obj = parsed.as_object();
    auto text = [&obj](const char* key) { return std::string(obj.at(key).as_string().c_str()); };
    auto number = [&obj](const char* key) { return obj.at(key).to_number<std::uint64_t>(); };

    Subscription subscription;
    subscription.id = text("id");
    subscription.presentity = text("presentity");
    subscription.package = text("package");
    subscription.event_id = text("event_id");
    subscription.call_id = text("call_id");
    subscription.local_tag = text("local_tag");
    subscription.remote_tag = text("remote_tag");
    subscription.watcher_uri = text("watcher_uri");
    subscription.presentity_uri = text("presentity_uri");
    subscription.remote_target = text("remote_target");
    for (const auto& route : obj.at("route_set").as_array()) subscription.route_set.emplace_back(route.as_string().c_str());
    subscription.contact = text("contact");
    subscription.local_cseq = number("local_cseq");
    subscription.remote_cseq = number("remote_cseq");
    subscription.version = number("version");
    subscription.expires = static_cast<std::uint32_t>(number("expires"));
    subscription.expires_at = static_cast<std::time_t>(obj.at("expires_at").to_number<std::int64_t>());
    subscription.flow_id = text("flow_id");
    if (const auto* digest = obj.if_contains("digest"); digest && digest->is_string()) subscription.digest = digest->as_string().c_str();
    // The holder is the record's, which a move rewrites.
    subscription.node_id = record.node_id;
    return subscription;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace athenasip
