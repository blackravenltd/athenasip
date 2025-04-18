//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_core.h"

namespace athenasip {

SIPCore::SIPCore(std::shared_ptr<Logger> logger, std::shared_ptr<Version> version, std::shared_ptr<Config> config, std::shared_ptr<Registrar> _registrar)
    : _logger(logger), _version(version), _config(config), registrar(_registrar) {}

void SIPCore::process_message(std::shared_ptr<SIPMessage> message) {
  // 1. Get the transaction

  // Message must contain Via and CSeq to identify the transaction
  if (!message->header->contains("Via") || !message->header->contains("CSeq")) {
    _logger->info("[Request] - Incomplete Headers (No Via/CSeq) - Sending 400 Bad Request");
    // TODO: Reject the message with a 400.

    // response->header->add("Reason", "SIP ;cause=400 ;text=\"Incomplete Headers (Needs From, To, Call-ID, CSeq, Via, Max-Forwards)\"");
    // _send(message, 400, "Bad Request");

    return;
  }

  // Find or create the messag transaction
  auto transactionId = message->get_transaction_id();
  _logger->debug("[Request] - Transaction is " + transactionId);
  message->transaction = registrar->transaction_get(transactionId);
  if (!message->transaction) {
    message->transaction = std::make_shared<Transaction>();
    message->transaction->id = transactionId;
    message->transaction->session = message->session;
    registrar->transaction_register(transactionId, message->transaction);
  }

  // Event
}

}  // namespace athenasip
