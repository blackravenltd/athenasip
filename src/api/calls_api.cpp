//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "calls_api.h"

#include <boost/asio/post.hpp>
#include <mutex>
#include <utility>
#include <vector>

#include "../call.h"
#include "../core.h"
#include "../loggers/logger_scoped.h"
#include "../qualifier.h"
#include "../types/user.h"
#include "../util.h"
#include "api_json.h"

namespace athenasip::api {

namespace {

boost::json::value profile_json(const std::optional<media::Profile>& profile) {
  if (!profile) return nullptr;

  switch (*profile) {
    case media::Profile::WebRtc:
      return "webrtc";
    case media::Profile::PlainRtp:
      return "plain-rtp";
    case media::Profile::SrtpSdes:
      return "srtp-sdes";
    case media::Profile::Mirror:
      break;
  }
  return nullptr;
}

// A profile in the words the behaviour settings use, so what is reported can be set as is.
boost::json::value setting_json(const std::optional<media::Profile>& profile) {
  if (!profile) return nullptr;

  switch (*profile) {
    case media::Profile::WebRtc:
      return "webrtc";
    case media::Profile::PlainRtp:
      return "rtp";
    case media::Profile::SrtpSdes:
      return "srtp";
    case media::Profile::Mirror:
      return "mirror";
  }
  return nullptr;
}

boost::json::value time_json(std::time_t when) {
  if (when == 0) return nullptr;
  return boost::json::string(Util::to_iso8601(when));
}

// One Prometheus metric, text exposition format 0.0.4: a TYPE line and its samples.
void metric(std::string& out, const std::string& name, const std::string& type, const std::string& help) {
  out += "# HELP " + name + " " + help + "\n";
  out += "# TYPE " + name + " " + type + "\n";
}

}  // namespace

CallsAPI::CallsAPI(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Core> core, plugins::Executor executor)
    : _logger(std::make_shared<loggers::LoggerScoped>("calls_api", std::move(logger))), _core(core), _executor(std::move(executor)) {}

void CallsAPI::register_routes(Router& router) {
  using types::roles::view_cluster_status;
  auto self = shared_from_this();

  // Status, the role registrations take: who is on a call is what is going on, not
  // something being provisioned.
  router.add(http::verb::get, "/api/v1/calls", {view_cluster_status}, [self](RouteContext c) { self->_list(std::move(c)); });
  router.add(http::verb::get, "/api/v1/calls/{call}", {view_cluster_status}, [self](RouteContext c) { self->_get(std::move(c)); });
  router.add(http::verb::get, "/api/v1/media", {view_cluster_status}, [self](RouteContext c) { self->_media(std::move(c)); });
  router.add(http::verb::get, "/api/v1/media/reoffers", {view_cluster_status}, [self](RouteContext c) { self->_reoffers(std::move(c)); });
  router.add(http::verb::get, "/api/v1/qualify", {view_cluster_status}, [self](RouteContext c) { self->_qualify(std::move(c)); });

  // On the admin port and outside /api/v1, where Prometheus expects it. Behind the same
  // credential as everything else: what a node is carrying is not for anybody who asks.
  router.add(http::verb::get, "/metrics", {view_cluster_status}, [self](RouteContext c) { self->_metrics(std::move(c)); });
}

void CallsAPI::_list(RouteContext context) {
  auto core = _core.lock();
  if (!core) {
    write_error(context.response, http::status::service_unavailable, "unavailable", "the node is shutting down");
    return context.done();
  }

  auto self = shared_from_this();
  core->post([self, core, context]() mutable {
    self->_describe(core->call_list(), [context](boost::json::array calls) mutable {
      write_json(context.response, http::status::ok, calls);
      context.done();
    });
  });
}

void CallsAPI::_get(RouteContext context) {
  auto core = _core.lock();
  if (!core) {
    write_error(context.response, http::status::service_unavailable, "unavailable", "the node is shutting down");
    return context.done();
  }

  // Percent-decoded by the router after the path was split, so a Call-ID holding a "/"
  // (RFC 3261 25.1 allows one) arrives here whole.
  const auto id = context.parameter("call");
  auto self = shared_from_this();

  core->post([self, core, context, id]() mutable {
    auto call = core->call_get(id);

    if (!call) {
      return boost::asio::post(self->_executor, [context]() mutable {
        write_error(context.response, http::status::not_found, "not_found", "no live call with that Call-ID");
        context.done();
      });
    }

    self->_describe({call}, [context](boost::json::array calls) mutable {
      write_json(context.response, http::status::ok, calls.empty() ? boost::json::value(nullptr) : calls[0]);
      context.done();
    });
  });
}

void CallsAPI::_describe(std::vector<std::shared_ptr<Call>> calls, std::function<void(boost::json::array)> then) {
  auto core = _core.lock();

  // Gathered as the engine answers, which is on the executor and may be in any order, and
  // handed on once every call has its media.
  struct Gather {
    std::mutex mutex;
    boost::json::array calls;
    std::size_t waiting = 0;
    std::function<void(boost::json::array)> then;
  };

  auto gather = std::make_shared<Gather>();
  gather->then = std::move(then);

  // Read on the strand, which is where a Call is written.
  for (const auto& call : calls) gather->calls.push_back(_call_json(*call));

  auto engine = core ? core->media : nullptr;
  const auto engine_name = engine ? engine->name() : std::string();

  std::vector<std::size_t> anchored;
  for (std::size_t i = 0; i < calls.size(); ++i) {
    if (engine && calls[i]->media_policy.anchor) anchored.push_back(i);
  }

  if (anchored.empty()) {
    return boost::asio::post(_executor, [gather]() { gather->then(std::move(gather->calls)); });
  }

  gather->waiting = anchored.size();

  for (const auto index : anchored) {
    engine->query(_executor, calls[index], [gather, index, engine_name](plugins::Result<std::string> document) {
      std::lock_guard lock(gather->mutex);

      gather->calls[index].as_object()["media"] = _media_json(document.ok ? document.value : std::string("{}"), engine_name);
      if (--gather->waiting == 0) gather->then(std::move(gather->calls));
    });
  }
}

boost::json::object CallsAPI::_call_json(const Call& call) {
  boost::json::object out;
  out["id"] = call.id;
  out["state"] = Call::state_to_string(call.state);
  out["created_at"] = time_json(call.created_at);
  out["answered_at"] = time_json(call.answered_at);

  boost::json::array participants;
  for (const auto& participant : call.participants) {
    boost::json::object entry;
    entry["identity"] = participant.identity && participant.identity->uri ? participant.identity->uri->to_string() : std::string();
    entry["originator"] = participant.originator;
    entry["profile"] = profile_json(participant.profile);
    participants.push_back(std::move(entry));
  }
  out["participants"] = std::move(participants);

  // Until the engine says otherwise: a call nothing anchors has no media to describe.
  out["media"] = nullptr;
  return out;
}

// The engine's own query document, cut down to what the API promises. A leg's counts pass
// through as the engine gave them, so a direction it did not report stays absent rather
// than reading as zero; "participant" is null because a relay knows an end by the address
// its packets come from, which behind a NAT is not one any participant described.
boost::json::value CallsAPI::_media_json(const std::string& document, const std::string& engine) {
  boost::json::object out;
  out["engine"] = engine;
  out["idle_seconds"] = nullptr;
  out["legs"] = boost::json::array();

  boost::system::error_code ec;
  const auto parsed = boost::json::parse(document, ec);
  if (ec || !parsed.is_object()) return out;

  const auto& from = parsed.as_object();

  if (const auto* idle = from.if_contains("idle_seconds"); idle && idle->is_int64()) out["idle_seconds"] = idle->as_int64();

  if (const auto* legs = from.if_contains("legs"); legs && legs->is_array()) {
    boost::json::array copied;
    for (const auto& leg : legs->as_array()) {
      if (!leg.is_object()) continue;

      boost::json::object entry;
      entry["participant"] = nullptr;
      for (const auto* key : {"packets_in", "bytes_in", "packets_out", "bytes_out"}) {
        if (const auto* value = leg.as_object().if_contains(key); value && value->is_int64()) entry[key] = value->as_int64();
      }
      copied.push_back(std::move(entry));
    }
    out["legs"] = std::move(copied);
  }

  return out;
}

void CallsAPI::_media(RouteContext context) {
  auto core = _core.lock();
  if (!core) {
    write_error(context.response, http::status::service_unavailable, "unavailable", "the node is shutting down");
    return context.done();
  }

  auto self = shared_from_this();
  core->post([self, core, context]() mutable {
    boost::json::object out;
    auto engine = core->media;

    // The engine's name and what it can do, never its URL: that carries an address and,
    // for some drivers, credentials.
    out["engine"] = engine ? boost::json::value(engine->name()) : boost::json::value(nullptr);
    out["connected"] = engine && engine->is_connected();

    boost::json::array capabilities;
    if (engine) {
      const auto can = engine->capabilities();
      if (can.bridge) capabilities.push_back("bridge");
      if (can.conference) capabilities.push_back("conference");
      if (can.record) capabilities.push_back("record");
      if (can.transcode) capabilities.push_back("transcode");
    }
    out["capabilities"] = std::move(capabilities);

    boost::asio::post(self->_executor, [context, out = std::move(out)]() mutable {
      write_json(context.response, http::status::ok, out);
      context.done();
    });
  });
}

// The accounts this node offered the other profile after a 488, and what came of it. The node
// suggests and the operator decides: the suggestion is the value the account's behaviour
// would take, and nothing sets it.
void CallsAPI::_reoffers(RouteContext context) {
  auto core = _core.lock();
  if (!core) {
    write_error(context.response, http::status::service_unavailable, "unavailable", "the node is shutting down");
    return context.done();
  }

  auto self = shared_from_this();
  core->post([self, core, context]() mutable {
    boost::json::array out;

    for (const auto& reoffer : core->reoffers().list()) {
      boost::json::object entry;
      entry["account"] = reoffer.account;
      entry["rejected"] = setting_json(reoffer.rejected);
      entry["took"] = setting_json(reoffer.took);
      entry["count"] = reoffer.count;
      entry["last_at"] = time_json(reoffer.last_at);
      entry["suggested_media_profile"] = setting_json(reoffer.took);
      out.push_back(std::move(entry));
    }

    boost::asio::post(self->_executor, [context, out = std::move(out)]() mutable {
      write_json(context.response, http::status::ok, out);
      context.done();
    });
  });
}

// The registered clients this node is sending OPTIONS to, and what each has answered. Per
// node: only the node holding a client's flow can probe it.
void CallsAPI::_qualify(RouteContext context) {
  auto core = _core.lock();
  if (!core) {
    write_error(context.response, http::status::service_unavailable, "unavailable", "the node is shutting down");
    return context.done();
  }

  auto self = shared_from_this();
  core->post([self, core, context]() mutable {
    boost::json::array out;

    for (const auto& probe : core->qualifier()->list()) {
      boost::json::object entry;
      entry["account"] = probe.aor;
      entry["contact"] = probe.contact ? boost::json::value(probe.contact->to_string()) : boost::json::value(nullptr);
      entry["interval"] = probe.interval;
      entry["answered_at"] = time_json(probe.answered_at);
      entry["unanswered"] = probe.unanswered;
      entry["said_media_profile"] = setting_json(probe.said);
      out.push_back(std::move(entry));
    }

    boost::asio::post(self->_executor, [context, out = std::move(out)]() mutable {
      write_json(context.response, http::status::ok, out);
      context.done();
    });
  });
}

void CallsAPI::_metrics(RouteContext context) {
  auto core = _core.lock();
  if (!core) {
    write_error(context.response, http::status::service_unavailable, "unavailable", "the node is shutting down");
    return context.done();
  }

  auto self = shared_from_this();
  core->post([self, core, context]() mutable {
    std::string body;

    metric(body, "athenasip_calls_active", "gauge", "Calls this node is carrying.");
    body += "athenasip_calls_active " + std::to_string(core->call_count()) + "\n";

    metric(body, "athenasip_dialogs_active", "gauge", "Dialogs this node is on the path of.");
    body += "athenasip_dialogs_active " + std::to_string(core->dialogs()->size()) + "\n";

    metric(body, "athenasip_channels_open", "gauge", "Transport flows this node holds, by transport.");
    for (const auto& [transport, count] : core->channel_counts()) {
      body += "athenasip_channels_open{transport=\"" + transport + "\"} " + std::to_string(count) + "\n";
    }

    metric(body, "athenasip_transactions_active", "gauge", "SIP transactions in progress.");
    body += "athenasip_transactions_active " + std::to_string(core->transaction_count()) + "\n";

    // Only from an engine that can count. A zero from one that cannot would read as a
    // node that carried nothing.
    if (core->media) {
      if (const auto relayed = core->media->packets_relayed()) {
        metric(body, "athenasip_media_packets_relayed_total", "counter", "Media packets the engine has sent on since it started.");
        body += "athenasip_media_packets_relayed_total " + std::to_string(*relayed) + "\n";
      }
    }

    boost::asio::post(self->_executor, [context, body = std::move(body)]() mutable {
      context.response->result(http::status::ok);
      context.response->set(http::field::content_type, "text/plain; version=0.0.4");
      context.response->body() = std::move(body);
      context.response->prepare_payload();
      context.done();
    });
  });
}

}  // namespace athenasip::api
