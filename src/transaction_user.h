//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <memory>

#include "sip_message.h"
#include "transactions/transaction_base.h"

namespace athenasip {

// The transaction user: the layer above RFC 3261 section 17.
//
// A TU receives requests the transaction layer has already de-duplicated and answers
// through the transaction it is given, never the transport: retransmission belongs to the
// transaction, SIP semantics to the TU. Registrar (section 10) is the TU for REGISTER and
// Proxy (section 16) for everything else.
//
// A TU answers from async handlers that run after on_request has returned, so it is
// shared and holds a reference to itself across that gap.
class TransactionUser : public std::enable_shared_from_this<TransactionUser> {
 public:
  virtual ~TransactionUser() = default;

  // transaction is the server transaction the request arrived on. It is null only for the
  // ACK to a 2xx, which travels outside any transaction (RFC 3261 17.1.1.3).
  virtual void on_request(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction) = 0;
};

}  // namespace athenasip
