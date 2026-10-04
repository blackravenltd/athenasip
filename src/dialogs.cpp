//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "dialogs.h"

#include <algorithm>
#include <utility>

#include "channel.h"
#include "headers/cseq_header.h"
#include "headers/session_expires_header.h"
#include "headers/sip_identity_header.h"
#include "loggers/logger_scoped.h"
#include "util.h"

namespace athenasip {

using athenasip::headers::CSeqHeader;
using athenasip::headers::SessionExpiresHeader;
using athenasip::headers::SIPIdentityHeader;
using athenasip::types::Dialog;

namespace {

std::shared_ptr<SIPIdentity> identity_of(const std::shared_ptr<SIPMessage>& message, const std::string& field) {
  if (!message->header->contains(field)) return nullptr;

  auto header = message->header->headers_map[field][0]->as<SIPIdentityHeader>();
  return header == nullptr ? nullptr : header->value;
}

std::string tag_of(const std::shared_ptr<SIPMessage>& message, const std::string& field) {
  auto identity = identity_of(message, field);
  if (!identity) return "";

  const auto tag = identity->tags.find("tag");
  return tag == identity->tags.end() ? "" : tag->second;
}

std::string call_id_of(const std::shared_ptr<SIPMessage>& message) {
  if (!message->header->contains("Call-ID")) return "";
  return message->header->headers_map["Call-ID"][0]->to_string();
}

std::uint64_t cseq_of(const std::shared_ptr<SIPMessage>& message) {
  if (!message->header->contains("CSeq")) return 0;

  auto cseq = message->header->headers_map["CSeq"][0]->as<CSeqHeader>();
  return cseq == nullptr ? 0 : cseq->sequence;
}

// RFC 3261 12.1.1 remote target: the Contact. "*" is the registrar's wildcard (20.10), not
// a target.
std::shared_ptr<SIPUri> target_of(const std::shared_ptr<SIPMessage>& message) {
  auto contact = identity_of(message, "Contact");
  if (!contact || contact->star) return nullptr;
  return contact->uri;
}

std::vector<std::shared_ptr<SIPUri>> route_set_of(const std::shared_ptr<SIPMessage>& message) {
  std::vector<std::shared_ptr<SIPUri>> route_set;

  if (!message->header->contains("Record-Route")) return route_set;

  for (const auto& value : message->header->headers_map["Record-Route"]) {
    auto header = value->as<SIPIdentityHeader>();
    if (header == nullptr || header->value == nullptr || !header->value->uri) continue;
    route_set.push_back(header->value->uri);
  }

  return route_set;
}

// RFC 3261 12.1.1: secure only if the request came over TLS and its Request-URI is sips.
bool is_secure(const std::shared_ptr<SIPMessage>& request) {
  if (!request->header->request_uri || Util::to_lower(request->header->request_uri->scheme) != "sips") return false;

  auto channel = request->channel.lock();
  if (!channel || !channel->_connection) return false;

  const auto transport = Util::to_lower(channel->_connection->transport_name());
  return transport == "tls" || transport == "wss";
}

bool is_2xx(int code) { return code >= 200 && code < 300; }

// RFC 4028 section 4. Null when the header is absent, which is not an error.
headers::SessionExpiresHeader* session_expires_of(const std::shared_ptr<SIPMessage>& message) {
  if (!message->header->contains("Session-Expires")) return nullptr;
  return message->header->headers_map["Session-Expires"][0]->as<SessionExpiresHeader>();
}

}  // namespace

Dialogs::Dialogs(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<TimerSource> timers)
    : _logger(std::make_shared<loggers::LoggerScoped>("dialogs", std::move(logger))), _timers(std::move(timers)) {}

void Dialogs::observe_request(const std::shared_ptr<SIPMessage>& request) {
  const auto call_id = call_id_of(request);
  if (call_id.empty()) return;

  const auto from_tag = tag_of(request, "From");
  const auto to_tag = tag_of(request, "To");
  const auto& method = request->header->request_method;

  // RFC 3261 12.1: an INVITE with no To tag is a new call attempt; record the caller's half.
  if (method == "INVITE" && to_tag.empty()) {
    auto& branches = _by_call_id[call_id];

    // Retransmissions are absorbed by the server transaction (17.2.1), so a repeat here is a
    // caller reusing the Call-ID; the recorded attempt stands.
    for (const auto& existing : branches) {
      if (existing->caller_tag == from_tag) return;
    }

    auto dialog = std::make_shared<Dialog>();
    dialog->call_id = call_id;
    dialog->caller_tag = from_tag;
    dialog->caller = identity_of(request, "From");
    dialog->callee = identity_of(request, "To");
    dialog->caller_target = target_of(request);
    dialog->caller_cseq = cseq_of(request);
    dialog->secure = is_secure(request);
    dialog->created_at = std::time(nullptr);
    if (const auto channel = request->channel.lock()) dialog->from_node = channel->peer_node();

    branches.push_back(dialog);

    _logger->debug("Call attempt " + call_id + " from " + from_tag);
    _changed(dialog);
    return;
  }

  auto dialog = find(request);
  if (!dialog) return;

  // 12.2.1.1: each end has its own CSeq sequence.
  const auto cseq = cseq_of(request);

  if (dialog->is_from_caller(from_tag)) {
    if (cseq > dialog->caller_cseq) dialog->caller_cseq = cseq;
  } else if (cseq > dialog->callee_cseq) {
    dialog->callee_cseq = cseq;
  }

  // RFC 3261 15.1: a BYE from either end terminates the dialog.
  if (method == "BYE") return _terminate(dialog, "BYE");

  // RFC 3261 9.1: a CANCEL ends an early attempt and has no effect on an answered call.
  if (method == "CANCEL") {
    if (dialog->state == Dialog::State::Early) _terminate(dialog, "CANCEL");
    return;
  }

  // A re-INVITE or UPDATE may move the remote target (12.2.1.1) and refresh the session
  // (RFC 4028).
  if (method == "INVITE" || method == "UPDATE") {
    if (auto target = target_of(request)) {
      if (dialog->is_from_caller(from_tag)) {
        dialog->caller_target = target;
      } else {
        dialog->callee_target = target;
      }
    }

    // RFC 4028 section 7: a re-INVITE or UPDATE in the dialog is the refresh.
    if (dialog->session_interval > 0) {
      dialog->session_deadline = _timers->now() + std::chrono::seconds(dialog->session_interval);
      _reschedule_sweep();
    }

    _changed(dialog);
  }
}

void Dialogs::observe_response(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<SIPMessage>& response) {
  // Only an INVITE creates a dialog (12.1).
  if (request->header->request_method != "INVITE") return;

  const auto call_id = call_id_of(request);
  if (call_id.empty()) return;

  const auto code = response->header->response_code;
  const auto callee_tag = tag_of(response, "To");

  // 12.1: no To tag means no dialog yet, only an attempt that may still fail.
  if (callee_tag.empty()) {
    if (code >= 300) {
      auto attempt = find(request);
      if (attempt) _terminate(attempt, "final response with no To tag");
    }
    return;
  }

  auto dialog = _for_tag(call_id, callee_tag);
  if (!dialog) return;

  if (code >= 300) return _terminate(dialog, "non-2xx final response");

  // A provisional with a To tag makes an early dialog; a 2xx confirms it (12.1). The callee's
  // target and route set are recorded in both cases.
  if (auto target = target_of(response)) dialog->callee_target = target;
  if (auto route_set = route_set_of(response); !route_set.empty()) dialog->route_set = std::move(route_set);

  dialog->callee = identity_of(response, "To");
  dialog->callee_cseq = cseq_of(response);

  if (is_2xx(code) && dialog->state != Dialog::State::Confirmed) {
    dialog->state = Dialog::State::Confirmed;
    dialog->confirmed_at = std::time(nullptr);
    dialog->confirmed_monotonic = _timers->now();

    // RFC 4028 section 7.1: the 2xx carries the agreed interval; the INVITE's was only a
    // request.
    if (auto* session = session_expires_of(response); session != nullptr && session->delta_seconds > 0) {
      dialog->session_interval = session->delta_seconds;
      dialog->refresher = session->refresher;
      dialog->session_deadline = _timers->now() + std::chrono::seconds(dialog->session_interval);

      _reschedule_sweep();
    }

    _logger->info("Dialog " + dialog->id() + " confirmed");
  }

  _changed(dialog);
}

void Dialogs::observe_branch_failure(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<SIPMessage>& response) {
  if (request->header->request_method != "INVITE") return;

  const auto callee_tag = tag_of(response, "To");
  if (callee_tag.empty()) return;

  auto search = _by_call_id.find(call_id_of(request));
  if (search == _by_call_id.end()) return;

  auto& branches = search->second;
  auto failed = std::find_if(branches.begin(), branches.end(), [&callee_tag](const auto& dialog) { return dialog->callee_tag == callee_tag; });
  if (failed == branches.end() || (*failed)->state != Dialog::State::Early) return;

  // Another branch's dialog carries the call on, so this one is dropped without a change
  // notification.
  if (branches.size() > 1) {
    _logger->debug("Early dialog " + (*failed)->id() + " ended with its branch");
    branches.erase(failed);
    return;
  }

  // The only dialog is the attempt itself: clear the callee half so the next branch can
  // fill it.
  auto& attempt = *failed;
  attempt->callee_tag.clear();
  attempt->callee = identity_of(request, "To");
  attempt->callee_target.reset();
  attempt->route_set.clear();
  attempt->callee_cseq = 0;
}

std::shared_ptr<Dialog> Dialogs::_for_tag(const std::string& call_id, const std::string& callee_tag) {
  auto search = _by_call_id.find(call_id);
  if (search == _by_call_id.end()) return nullptr;

  auto& branches = search->second;

  for (const auto& dialog : branches) {
    if (dialog->callee_tag == callee_tag) return dialog;
  }

  // The first tag completes the existing attempt. A later one (a fork with several answering
  // branches) gets its own dialog copied from the caller's half (12.1).
  for (const auto& dialog : branches) {
    if (dialog->callee_tag.empty()) {
      dialog->callee_tag = callee_tag;
      return dialog;
    }
  }

  if (branches.empty()) return nullptr;

  auto forked = std::make_shared<Dialog>(*branches.front());
  forked->callee_tag = callee_tag;
  forked->state = Dialog::State::Early;
  forked->confirmed_at = 0;
  forked->confirmed_monotonic = {};
  forked->terminated_at = 0;

  branches.push_back(forked);
  return forked;
}

std::shared_ptr<Dialog> Dialogs::find(const std::shared_ptr<SIPMessage>& message) const {
  const auto call_id = call_id_of(message);
  if (call_id.empty()) return nullptr;

  auto search = _by_call_id.find(call_id);
  if (search == _by_call_id.end()) return nullptr;

  const auto from_tag = tag_of(message, "From");
  const auto to_tag = tag_of(message, "To");

  for (const auto& dialog : search->second) {
    if (dialog->matches(call_id, from_tag, to_tag)) return dialog;
  }

  // A caller's request with no To tag belongs to the attempt even after the callee has
  // answered with one: a CANCEL carries the INVITE's tagless To (9.1).
  if (to_tag.empty()) {
    for (const auto& dialog : search->second) {
      if (dialog->caller_tag == from_tag) return dialog;
    }
  }

  return nullptr;
}

std::vector<std::shared_ptr<Dialog>> Dialogs::all() const {
  std::vector<std::shared_ptr<Dialog>> dialogs;

  for (const auto& [call_id, branches] : _by_call_id) {
    for (const auto& dialog : branches) dialogs.push_back(dialog);
  }

  return dialogs;
}

std::size_t Dialogs::size() const {
  std::size_t count = 0;
  for (const auto& [call_id, branches] : _by_call_id) count += branches.size();
  return count;
}

void Dialogs::terminate(const std::shared_ptr<types::Dialog>& dialog) { _terminate(dialog, "torn down"); }

// RFC 4028 section 8: on expiry a proxy discards its session state. It does not send BYE;
// the endpoints run their own timers and send their own.
void Dialogs::_sweep() {
  const auto now = _timers->now();

  std::vector<std::shared_ptr<Dialog>> lapsed;

  for (const auto& [call_id, branches] : _by_call_id) {
    for (const auto& dialog : branches) {
      if (dialog->session_interval > 0 && dialog->session_deadline <= now) lapsed.push_back(dialog);
    }
  }

  for (const auto& dialog : lapsed) _terminate(dialog, "session expired");

  _reschedule_sweep();
}

void Dialogs::_reschedule_sweep() {
  if (_sweep_timer) {
    _sweep_timer->cancel();
    _sweep_timer.reset();
  }

  bool found = false;
  std::chrono::steady_clock::time_point earliest{};

  for (const auto& [call_id, branches] : _by_call_id) {
    for (const auto& dialog : branches) {
      if (dialog->session_interval == 0) continue;
      if (!found || dialog->session_deadline < earliest) {
        earliest = dialog->session_deadline;
        found = true;
      }
    }
  }

  // No dialog negotiated a session timer, so none is armed.
  if (!found) return;

  const auto now = _timers->now();
  const auto delay = earliest <= now ? std::chrono::milliseconds(0) : std::chrono::duration_cast<std::chrono::milliseconds>(earliest - now);

  // Weak, so a timer that outlives shutdown does not fire into a destroyed table.
  std::weak_ptr<Dialogs> weak_self = weak_from_this();
  _sweep_timer = _timers->schedule(delay, [weak_self]() {
    if (auto self = weak_self.lock()) self->_sweep();
  });
}

void Dialogs::_terminate(const std::shared_ptr<Dialog>& dialog, const char* reason) {
  if (!dialog || dialog->state == Dialog::State::Terminated) return;

  dialog->state = Dialog::State::Terminated;
  dialog->terminated_at = std::time(nullptr);

  _logger->info("Dialog " + (dialog->id().empty() ? dialog->call_id : dialog->id()) + " terminated - " + reason);

  // Notify before erasing: the callback writes the call record and releases media.
  _changed(dialog);

  auto search = _by_call_id.find(dialog->call_id);
  if (search == _by_call_id.end()) return;

  auto& branches = search->second;
  const bool had_timer = dialog->session_interval > 0;

  std::erase(branches, dialog);

  if (branches.empty()) _by_call_id.erase(search);

  // The sweep timer may have been armed for this dialog.
  if (had_timer) _reschedule_sweep();
}

void Dialogs::_changed(const std::shared_ptr<Dialog>& dialog) {
  if (_on_change) _on_change(dialog);
}

}  // namespace athenasip
