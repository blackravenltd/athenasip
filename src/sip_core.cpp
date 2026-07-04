//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_core.h"

#include "util.h"

namespace athenasip {

SIPCore::SIPCore(std::shared_ptr<Logger> logger, std::shared_ptr<Version> version, std::shared_ptr<Config> config, std::shared_ptr<Registrar> _registrar)
    : _logger(logger), _version(version), _config(config), registrar(_registrar) {}

void SIPCore::process_message(std::shared_ptr<SIPMessage> message) {
  // Message must contain Via and CSeq to identify the transaction
  if (!message->header->contains("Via") || !message->header->contains("CSeq")) {
    _logger->info("[Request] - Incomplete Headers (No Via/CSeq) - Sending 400 Bad Request");

    // Send 400 Bad Request
    auto response = message->generate_response();
    response->header->add("Reason", "SIP ;cause=400 ;text=\"Incomplete Headers (Needs From, To, Call-ID, CSeq, Via, Max-Forwards)\"");
    response->header->response_code = 400;
    response->header->response_message = "Bad Request";
    response->channel->send(response);

    return;
  }

  // Find or create the message transaction
  auto transactionId = message->get_transaction_id();
  _logger->debug("[Request] - Transaction is " + transactionId);
  message->transaction = registrar->transaction_get(transactionId);
  if (!message->transaction) {
    message->transaction = std::make_shared<Transaction>(_logger, message->channel, registrar, Transaction::Direction::Incoming, transactionId);
    message->transaction->type = (message->header->type == SIPHeader::Type::Request && message->header->request_method == "INVITE")
                                     ? Transaction::Type::INVITE
                                     : Transaction::Type::NonINVITE;
    message->transaction->start(_config->sip_timer_t1_rtt_ms);
  } else {
    message->transaction->reset_timers();
  }

  // Parse the Message in the context of the transaction
  message->transaction->receive_message(message);
}

}  // namespace athenasip
