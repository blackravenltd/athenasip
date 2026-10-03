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

// RFC 3261 12.1.1 remote target: the Contact the end offered. A Contact of "*" is the
// registrar's wildcard (20.10) and is not a target.
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

// RFC 3261 12.1.1: a dialog is secure when the request was sent over TLS and the
// Request-URI was a sips URI. Both are required - TLS on the hop this node saw says
// nothing about the hops it did not.
bool is_secure(const std::shared_ptr<SIPMessage>& request) {
  if (!request->header->request_uri || Util::to_lower(request->header->request_uri->scheme) != "sips") return false;

  auto channel = request->channel.lock();
  if (!channel || !channel->_connection) return false;

  const auto transport = Util::to_lower(channel->_connection->transport_name());
  return transport == "tls" || transport == "wss";
}

bool is_2xx(int code) { return code >= 200 && code < 300; }

// RFC 4028 section 4. Null when the message carries no such field, which is the common
// case and is not an error: session timers are an extension both ends have to offer.
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

  // RFC 3261 12.1: an INVITE with no To tag is outside any dialog, so it is a new call
  // attempt. The caller's half of the dialog is everything that can be known before the
  // callee has said anything at all.
  if (method == "INVITE" && to_tag.empty()) {
    auto& branches = _by_call_id[call_id];

    // A retransmission never reaches here - the server transaction absorbs it (17.2.1) -
    // so a second attempt on one Call-ID is a caller that reused it, and the one already
    // recorded stands.
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

  // 12.2.1.1: each end has its own sequence, and which one this request advances is a
  // question of which end it came from.
  const auto cseq = cseq_of(request);

  if (dialog->is_from_caller(from_tag)) {
    if (cseq > dialog->caller_cseq) dialog->caller_cseq = cseq;
  } else if (cseq > dialog->callee_cseq) {
    dialog->callee_cseq = cseq;
  }

  // RFC 3261 15.1: a BYE ends the dialog, whichever end sends it. This is the whole
  // reason the node tracks dialogs at all - it is where media is released and the call
  // record closed.
  if (method == "BYE") return _terminate(dialog, "BYE");

  // RFC 3261 9.1: a CANCEL ends the attempt it names. It has no effect on one already
  // answered, which is the race where the callee picked up as the caller gave up - the
  // call is up, and a BYE is what ends it now.
  if (method == "CANCEL") {
    if (dialog->state == Dialog::State::Early) _terminate(dialog, "CANCEL");
    return;
  }

  // A re-INVITE or an UPDATE may move the remote target (12.2.1.1) and refresh the
  // session (RFC 4028).
  if (method == "INVITE" || method == "UPDATE") {
    if (auto target = target_of(request)) {
      if (dialog->is_from_caller(from_tag)) {
        dialog->caller_target = target;
      } else {
        dialog->callee_target = target;
      }
    }

    // RFC 4028 section 7: the refresh is a re-INVITE or an UPDATE inside the dialog.
    // A proxy takes the request itself as the proof it wants - a request travelling
    // between the two ends is both of them still being there, which is the only
    // question a node keeping state has to answer.
    if (dialog->session_interval > 0) {
      dialog->session_deadline = _timers->now() + std::chrono::seconds(dialog->session_interval);
      _reschedule_sweep();
    }

    _changed(dialog);
  }
}

void Dialogs::observe_response(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<SIPMessage>& response) {
  // Only an INVITE creates a dialog (12.1). Responses to anything else move nothing
  // here, and a BYE's own 200 arrives after the dialog has already gone.
  if (request->header->request_method != "INVITE") return;

  const auto call_id = call_id_of(request);
  if (call_id.empty()) return;

  const auto code = response->header->response_code;
  const auto callee_tag = tag_of(response, "To");

  // 12.1 again: without a To tag the callee has not identified itself, so there is no
  // dialog yet - only an attempt that may still fail.
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

  // A provisional with a To tag is an early dialog; a 2xx confirms it (12.1). Either way
  // the callee has now said where it is and what route it wants, so both are recorded -
  // a call cancelled while ringing still has to release what the early dialog reserved.
  if (auto target = target_of(response)) dialog->callee_target = target;
  if (auto route_set = route_set_of(response); !route_set.empty()) dialog->route_set = std::move(route_set);

  dialog->callee = identity_of(response, "To");
  dialog->callee_cseq = cseq_of(response);

  if (is_2xx(code) && dialog->state != Dialog::State::Confirmed) {
    dialog->state = Dialog::State::Confirmed;
    dialog->confirmed_at = std::time(nullptr);
    dialog->confirmed_monotonic = _timers->now();

    // RFC 4028 section 7.1: the 2xx carries what the two ends settled on, which is the
    // only value worth recording - what the INVITE asked for is a request, not an
    // agreement, and the UAS is free to lower it.
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

  // Another branch's dialog carries the call on, and this one simply goes. Quietly: the
  // call record follows the dialog it is told about, and this one ending is not the call.
  if (branches.size() > 1) {
    _logger->debug("Early dialog " + (*failed)->id() + " ended with its branch");
    branches.erase(failed);
    return;
  }

  // The only one, which is the attempt itself with this branch's half filled in. It goes
  // back to being an attempt, ready for the next branch to give it a callee.
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

  // The first tag to arrive completes the attempt that is already there. A second, which
  // only happens when a fork had more than one branch answer, gets a dialog of its own
  // built from the same caller half (12.1).
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

  // A request from the caller with no To tag is one sent before the caller learned of
  // any tag, so it belongs to the attempt whether or not the callee has since answered
  // with one. The CANCEL is why this matters: it carries the INVITE's tagless To (9.1)
  // and still has to find the call it is cancelling after a 180 has gone back.
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

// RFC 4028 section 8: a proxy keeping session state uses the timer to decide when to
// discard it. Discard, and not a BYE to both ends: sending one would be acting as a user
// agent in a dialog this node is only on the path of, and section 16 is clear that a
// proxy is not that. The endpoints run their own timers and will each send their own
// BYE; what expiry means here is that this node stops holding the call open.
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

  // Nothing negotiated a timer, so nothing can lapse and no timer is armed. A node
  // whose endpoints do not use session timers pays nothing for this.
  if (!found) return;

  const auto now = _timers->now();
  const auto delay = earliest <= now ? std::chrono::milliseconds(0) : std::chrono::duration_cast<std::chrono::milliseconds>(earliest - now);

  // Weak: a timer outlives the node's shutdown otherwise, and fires into a table that
  // has gone.
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

  // The record goes out before the dialog does, because the callback is what writes the
  // call record and releases the media. After it returns, what is left in the table is
  // what is still live and nothing else.
  _changed(dialog);

  auto search = _by_call_id.find(dialog->call_id);
  if (search == _by_call_id.end()) return;

  auto& branches = search->second;
  const bool had_timer = dialog->session_interval > 0;

  std::erase(branches, dialog);

  if (branches.empty()) _by_call_id.erase(search);

  // The timer may have been armed for the dialog that just went.
  if (had_timer) _reschedule_sweep();
}

void Dialogs::_changed(const std::shared_ptr<Dialog>& dialog) {
  if (_on_change) _on_change(dialog);
}

}  // namespace athenasip
