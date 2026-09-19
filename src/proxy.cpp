//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "proxy.h"

#include <utility>

#include "channel.h"
#include "core.h"
#include "headers/uint_header.h"
#include "headers/via_header.h"
#include "loggers/logger_scoped.h"
#include "util.h"

namespace athenasip {

using athenasip::headers::UIntHeader;
using athenasip::headers::ViaHeader;

namespace {

constexpr std::uint64_t kDefaultMaxForwards = 70;

bool is_2xx(int code) { return code >= 200 && code < 300; }
bool is_final(int code) { return code >= 200; }

}  // namespace

Proxy::Proxy(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Core> core)
    : _logger(std::make_shared<loggers::LoggerScoped>("proxy", std::move(logger))), _core(core) {}

void Proxy::on_request(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction) {
  auto core = _core.lock();
  if (!core) return;

  if (!request->header->request_uri) {
    _logger->info("Request with no Request-URI - 400");
    return _send_status(transaction, request, 400, "Bad Request");
  }

  // RFC 3261 16.5: the request URI names the address of record, and the bindings the
  // registrar holds for it are the targets. Both reads are round trips, so target
  // determination finishes in a handler rather than before this function returns.
  auto identity = std::make_shared<SIPIdentity>(request->header->request_uri->to_string());
  auto self = shared_from_this();

  core->subscriber_get(identity, [this, self, request, transaction](plugins::Result<std::shared_ptr<types::Subscriber>> found) {
    auto core = _core.lock();
    if (!core) return;

    if (!found.ok) {
      _logger->error("Could not read the subscriber for " + request->header->request_uri->to_string() + " - " + found.error);
      return _send_status(transaction, request, 500, "Server Internal Error");
    }

    if (!found.value) {
      _logger->info("No subscriber for " + request->header->request_uri->to_string() + " - 404");
      return _send_status(transaction, request, 404, "Not Found");
    }

    auto subscriber = found.value;

    core->location_list(subscriber->id, [this, self, request, transaction, subscriber](plugins::Result<std::vector<types::Location>> bindings) {
      auto core = _core.lock();
      if (!core) return;

      if (!bindings.ok) {
        _logger->error("Could not read the bindings for " + request->header->request_uri->to_string() + " - " + bindings.error);
        return _send_status(transaction, request, 500, "Server Internal Error");
      }

      auto context = std::make_shared<Context>();
      context->request = request;
      context->server = transaction;
      context->targets = bindings.value;

      if (context->targets.empty()) {
        _logger->info("No bindings for " + request->header->request_uri->to_string() + " - 480");
        return _send_status(transaction, request, 480, "Temporarily Unavailable");
      }

      // One node, one flow per subscriber: the request goes back down the connection the
      // callee registered on. Per-binding flow routing is RFC 5626, and Location::flow_id
      // exists for it.
      auto channel = core->subscriber_get_channel(subscriber);
      if (!channel) {
        _logger->info("No live flow for " + request->header->request_uri->to_string() + " - 480");
        return _send_status(transaction, request, 480, "Temporarily Unavailable");
      }

      context->outbound = channel;

      _forward_next(context);
    });
  });
}

void Proxy::_forward_next(const std::shared_ptr<Context>& context) {
  auto core = _core.lock();
  if (!core) return;

  auto channel = context->outbound.lock();

  if (!channel || context->next >= context->targets.size()) {
    // Out of targets. RFC 3261 16.7: the best response seen goes back, and 480 stands in
    // when nothing answered at all.
    if (!context->server) return;

    if (context->best) {
      context->server->send(context->best);
    } else {
      _send_status(context->server, context->request, 480, "Temporarily Unavailable");
    }
    return;
  }

  const auto& target = context->targets[context->next++];

  if (!_prepare_forward(context->request, channel, target.contact)) {
    _logger->info("Max-Forwards exhausted - 483");
    return _send_status(context->server, context->request, 483, "Too Many Hops");
  }

  // The ACK for a 2xx travels outside any transaction (RFC 3261 17.1.1.3), so it is
  // written straight to the transport.
  if (!context->server) {
    channel->send(context->request);
    return;
  }

  core->client_transaction_start(
      context->request, channel, [this, context](std::shared_ptr<SIPMessage> response) { _on_response(context, response); },
      [this, context]() {
        // Timer B or F. RFC 3261 16.7: a branch that never answered is a 408, and the
        // next target gets its turn.
        _logger->info("No response from target - trying the next");

        auto timeout = context->request->generate_response();
        timeout->header->response_code = 408;
        timeout->header->response_message = "Request Timeout";

        if (!context->best) context->best = timeout;
        _forward_next(context);
      });
}

void Proxy::_on_response(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& response) {
  if (!context->server) return;

  // RFC 3261 16.7 step 3: the Via this node added on the way out comes off on the way
  // back, so what the caller sees is the chain it sent.
  if (response->header->contains("Via") && !response->header->headers_map["Via"].empty()) {
    auto top = response->header->headers_map["Via"][0];
    response->header->remove_value("Via", [&top](std::shared_ptr<Header> header) { return header == top; });
  }

  const int code = response->header->response_code;

  if (!is_final(code)) {
    context->server->send(response);
    return;
  }

  // A 2xx ends the search: there is an answer and forking stops (16.7 step 5).
  if (is_2xx(code)) {
    context->best = response;
    context->server->send(response);
    return;
  }

  // 6xx is a definitive refusal from the user and stops the search too (16.7 step 5).
  if (code >= 600) {
    context->best = response;
    context->server->send(response);
    return;
  }

  // Otherwise remember it and try the next binding. Serial forking: lowest code wins,
  // which for the codes that reach here is the closest to an answer.
  if (!context->best || code < context->best->header->response_code) context->best = response;

  _forward_next(context);
}

bool Proxy::_prepare_forward(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<Channel>& channel, const std::shared_ptr<SIPUri>& target) const {
  // RFC 3261 16.6 step 2: the Request-URI becomes the target this hop is for.
  if (target) request->header->request_uri = target;

  // Step 3: Max-Forwards is decremented, and a request that has run out cannot be
  // forwarded (16.3 rule 3 answers it 483).
  if (request->header->contains("Max-Forwards")) {
    auto max_forwards = request->header->headers_map["Max-Forwards"][0]->as<UIntHeader>();
    if (max_forwards != nullptr) {
      if (max_forwards->value == 0) return false;
      max_forwards->value--;
    }
  } else {
    request->header->add("Max-Forwards", std::make_shared<UIntHeader>(kDefaultMaxForwards));
  }

  // Step 8: this node's Via goes on top, with a branch that identifies this client
  // transaction and nothing else (8.1.1.7). The transport is the one the request is
  // actually going out on (18.1.1).
  if (!channel->_connection) return false;

  const auto local = channel->_connection->local_endpoint();
  const auto branch = Util::generate_random_string("z9hG4bK", 16);

  auto via = std::make_shared<ViaHeader>("SIP/2.0/" + Util::to_upper(channel->_connection->transport_name()) + " " + local.address().to_string() + ":" +
                                         std::to_string(local.port()) + ";branch=" + branch);

  request->header->add_start("Via", via);
  request->branch = branch;

  return true;
}

void Proxy::on_cancel(std::shared_ptr<SIPMessage> cancel, std::shared_ptr<transactions::TransactionBase> cancel_transaction,
                      std::shared_ptr<transactions::TransactionBase> invite_transaction) {
  // RFC 3261 9.2: the CANCEL is answered on its own transaction whether or not it names
  // anything we still hold.
  if (cancel_transaction) {
    auto ok = cancel->generate_response();
    ok->header->response_code = 200;
    ok->header->response_message = "OK";
    cancel_transaction->send(ok);
  }

  if (!invite_transaction) {
    _logger->info("CANCEL for an unknown transaction - answered 200 and dropped");
    return;
  }

  // The INVITE it cancels ends with 487, which is what the caller is waiting for.
  auto terminated = cancel->generate_response();
  terminated->header->response_code = 487;
  terminated->header->response_message = "Request Terminated";

  invite_transaction->send(terminated);
}

void Proxy::_send_status(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request, std::uint16_t code,
                         const std::string& reason) {
  if (!transaction) return;

  auto response = request->generate_response();
  response->header->response_code = code;
  response->header->response_message = reason;

  transaction->send(response);
}

}  // namespace athenasip
