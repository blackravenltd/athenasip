//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "dialog.h"

namespace athenasip::types {

std::string Dialog::id() const {
  if (call_id.empty() || caller_tag.empty() || callee_tag.empty()) return "";

  // Separated, so two different pairs of tags cannot spell the same string.
  return call_id + "|" + caller_tag + "|" + callee_tag;
}

bool Dialog::matches(const std::string& message_call_id, const std::string& from_tag, const std::string& to_tag) const {
  if (message_call_id != call_id) return false;

  // RFC 3261 12.2.2, seen from a proxy: the tags arrive in either order, depending on which end sent the request.
  if (from_tag == caller_tag && to_tag == callee_tag) return true;
  if (from_tag == callee_tag && to_tag == caller_tag) return true;

  return false;
}

bool Dialog::is_from_caller(const std::string& from_tag) const { return !caller_tag.empty() && from_tag == caller_tag; }

namespace {

const char* state_name(Dialog::State state) {
  switch (state) {
    case Dialog::State::Early:
      return "early";
    case Dialog::State::Confirmed:
      return "confirmed";
    case Dialog::State::Terminated:
      return "terminated";
  }
  return "early";
}

std::string text_at(const boost::json::object& object, const char* key) {
  const auto* value = object.if_contains(key);
  return value != nullptr && value->is_string() ? std::string(value->as_string()) : std::string();
}

std::int64_t number_at(const boost::json::object& object, const char* key) {
  const auto* value = object.if_contains(key);
  return value != nullptr && value->is_number() ? value->to_number<std::int64_t>() : 0;
}

}  // namespace

boost::json::object Dialog::to_json() const {
  boost::json::object object;
  object["call_id"] = call_id;
  object["caller_tag"] = caller_tag;
  object["callee_tag"] = callee_tag;
  object["from_node"] = from_node;
  object["caller"] = caller && caller->uri ? caller->uri->to_string() : "";
  object["callee"] = callee && callee->uri ? callee->uri->to_string() : "";
  object["caller_target"] = caller_target ? caller_target->to_string() : "";
  object["callee_target"] = callee_target ? callee_target->to_string() : "";

  boost::json::array routes;
  for (const auto& route : route_set) {
    if (route) routes.push_back(boost::json::value(route->to_string()));
  }
  object["route_set"] = std::move(routes);

  object["caller_cseq"] = caller_cseq;
  object["callee_cseq"] = callee_cseq;
  object["secure"] = secure;
  object["state"] = state_name(state);
  object["created_at"] = static_cast<std::int64_t>(created_at);
  object["confirmed_at"] = static_cast<std::int64_t>(confirmed_at);
  object["terminated_at"] = static_cast<std::int64_t>(terminated_at);
  object["session_interval"] = session_interval;
  object["refresher"] = refresher;
  object["session_expires_at"] = static_cast<std::int64_t>(session_expires_at);
  object["trunk"] = trunk;
  object["cseq_offset"] = cseq_offset;
  object["revision"] = revision;
  return object;
}

std::shared_ptr<Dialog> Dialog::from_json(const boost::json::object& object) {
  auto dialog = std::make_shared<Dialog>();
  dialog->call_id = text_at(object, "call_id");
  dialog->caller_tag = text_at(object, "caller_tag");
  dialog->callee_tag = text_at(object, "callee_tag");
  dialog->from_node = text_at(object, "from_node");

  if (const auto caller = text_at(object, "caller"); !caller.empty()) dialog->caller = std::make_shared<SIPIdentity>(caller);
  if (const auto callee = text_at(object, "callee"); !callee.empty()) dialog->callee = std::make_shared<SIPIdentity>(callee);
  if (const auto target = text_at(object, "caller_target"); !target.empty()) dialog->caller_target = std::make_shared<SIPUri>(target);
  if (const auto target = text_at(object, "callee_target"); !target.empty()) dialog->callee_target = std::make_shared<SIPUri>(target);

  if (const auto* routes = object.if_contains("route_set"); routes != nullptr && routes->is_array()) {
    for (const auto& route : routes->as_array()) {
      if (route.is_string()) dialog->route_set.push_back(std::make_shared<SIPUri>(std::string(route.as_string())));
    }
  }

  dialog->caller_cseq = static_cast<std::uint64_t>(number_at(object, "caller_cseq"));
  dialog->callee_cseq = static_cast<std::uint64_t>(number_at(object, "callee_cseq"));
  if (const auto* secure = object.if_contains("secure"); secure != nullptr && secure->is_bool()) dialog->secure = secure->as_bool();

  const auto state = text_at(object, "state");
  dialog->state = state == "confirmed" ? State::Confirmed : state == "terminated" ? State::Terminated : State::Early;

  dialog->created_at = static_cast<std::time_t>(number_at(object, "created_at"));
  dialog->confirmed_at = static_cast<std::time_t>(number_at(object, "confirmed_at"));
  dialog->terminated_at = static_cast<std::time_t>(number_at(object, "terminated_at"));
  dialog->session_interval = static_cast<std::uint32_t>(number_at(object, "session_interval"));
  dialog->refresher = text_at(object, "refresher");
  dialog->session_expires_at = static_cast<std::time_t>(number_at(object, "session_expires_at"));
  dialog->trunk = text_at(object, "trunk");
  dialog->cseq_offset = static_cast<std::uint32_t>(number_at(object, "cseq_offset"));
  dialog->revision = static_cast<std::uint64_t>(number_at(object, "revision"));
  return dialog;
}

}  // namespace athenasip::types
