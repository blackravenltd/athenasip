//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "local_ua.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

#include "call.h"
#include "core.h"
#include "dialogs.h"
#include "util.h"

namespace athenasip {

LocalUA::LocalUA(std::shared_ptr<loggers::Logger> logger, std::weak_ptr<Core> core)
    : _logger(std::make_shared<loggers::LoggerScoped>("local_ua", std::move(logger))), _core(std::move(core)) {}

bool LocalUA::hang_up(const std::string& call_id, const std::string& reason) {
  auto core = _core.lock();
  if (!core) return false;

  // RFC 3261 15: a BYE belongs to a confirmed dialog. An early one ends with the caller's CANCEL.
  std::vector<std::shared_ptr<types::Dialog>> confirmed;
  for (const auto& dialog : core->dialogs()->all()) {
    if (dialog && dialog->call_id == call_id && dialog->state == types::Dialog::State::Confirmed) confirmed.push_back(dialog);
  }

  if (confirmed.empty()) return false;

  for (const auto& dialog : confirmed) {
    // Both are built before either is sent: the first BYE ends the dialog they are built from.
    auto to_callee = _request_to(*dialog, true, "BYE");
    auto to_caller = _request_to(*dialog, false, "BYE");

    _logger->info("Hanging up call " + call_id + " towards both ends - " + reason);

    for (auto& bye : {to_callee, to_caller}) {
      if (!bye) continue;

      core->local_request(bye, [this, self = shared_from_this(), call_id](std::shared_ptr<SIPMessage> response) {
        if (response && response->header && response->header->response_code >= 300) {
          _logger->info("A BYE for call " + call_id + " was answered " + std::to_string(response->header->response_code));
        }
      });
    }

    // An end with no target cannot be sent a BYE; the dialog still ends here.
    if (!to_callee || !to_caller) core->dialogs()->terminate(dialog);
  }

  return true;
}

bool LocalUA::key_pressed(const std::string& call_id, const std::string& source_tag, char digit, std::chrono::milliseconds duration) {
  auto core = _core.lock();
  if (!core) return false;

  auto call = core->call_get(call_id);
  if (!call) return false;

  std::shared_ptr<types::Dialog> dialog;
  for (const auto& candidate : core->dialogs()->all()) {
    if (candidate && candidate->call_id == call_id && candidate->state == types::Dialog::State::Confirmed) dialog = candidate;
  }
  if (!dialog) return false;

  // The press came from one leg; the other gets the INFO, if INFO is what it speaks.
  const bool from_caller = dialog->is_from_caller(source_tag);
  const auto far = call->participant_index(!from_caller);
  if (!far || call->participants[*far].dtmf != types::MediaPolicy::Dtmf::Info) return false;

  auto info = _request_to(*dialog, from_caller, "INFO", "application/dtmf-relay",
                          "Signal=" + std::string(1, digit) + "\r\nDuration=" + std::to_string(duration.count()) + "\r\n");
  if (!info) return false;

  _logger->debug("Sending " + std::string(1, digit) + " as INFO on call " + call_id);
  core->local_request(info, [this, self = shared_from_this(), call_id](std::shared_ptr<SIPMessage> response) {
    if (response && response->header && response->header->response_code >= 300) {
      _logger->info("A key press by INFO for call " + call_id + " was answered " + std::to_string(response->header->response_code));
    }
  });
  return true;
}

std::shared_ptr<SIPMessage> LocalUA::_request_to(const types::Dialog& dialog, bool callee, const std::string& method, const std::string& content_type,
                                                 const std::string& body) const {
  auto core = _core.lock();
  if (!core) return nullptr;

  const auto& target = callee ? dialog.callee_target : dialog.caller_target;
  const auto& to = callee ? dialog.callee : dialog.caller;
  const auto& from = callee ? dialog.caller : dialog.callee;

  if (!target || !to || !to->uri || !from || !from->uri) return nullptr;

  // 12.1.1: the route set is the Record-Route as recorded, the callee's end first. The far end's view runs from
  // itself: reversed for a BYE the caller sends. What lies between the far end and this node is behind it, so the
  // set starts at this node's own entries, which the proxy removes on its way to the flow token they carry.
  auto routes = dialog.route_set;
  if (callee) std::reverse(routes.begin(), routes.end());

  auto own = routes.begin();
  while (own != routes.end() && !core->names_this_node(**own)) ++own;
  routes.erase(routes.begin(), own);

  // 12.2.1.1: one past the far end's CSeq. A far end that has sent nothing has no sequence, and any value is in order.
  const auto cseq = (callee ? dialog.caller_cseq : dialog.callee_cseq) + 1;

  const auto branch = std::string("z9hG4bK") + Util::generate_random_string("", 16);
  const auto advertised = core->config->public_address().empty() ? std::string("athenasip.invalid") : core->config->public_address();

  std::string raw = method + " " + target->to_string() + " SIP/2.0\r\n";
  raw += "Via: SIP/2.0/UDP " + advertised + ";branch=" + branch + "\r\n";
  raw += "Max-Forwards: 70\r\n";
  for (const auto& route : routes) raw += "Route: <" + route->to_string() + ">\r\n";
  raw += "From: <" + from->uri->to_string() + ">;tag=" + (callee ? dialog.caller_tag : dialog.callee_tag) + "\r\n";
  raw += "To: <" + to->uri->to_string() + ">;tag=" + (callee ? dialog.callee_tag : dialog.caller_tag) + "\r\n";
  raw += "Call-ID: " + dialog.call_id + "\r\n";
  raw += "CSeq: " + std::to_string(cseq) + " " + method + "\r\n";
  if (!content_type.empty()) raw += "Content-Type: " + content_type + "\r\n";
  raw += "Content-Length: " + std::to_string(body.size()) + "\r\n";

  auto message = std::make_shared<SIPMessage>();
  message->header = std::make_shared<SIPHeader>(raw);
  message->body = body;
  message->branch = branch;
  return message;
}

}  // namespace athenasip
