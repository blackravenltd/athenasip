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
// A TU is handed requests the transaction layer has already de-duplicated, and answers
// through the transaction it was given rather than writing to the transport itself. That
// is the whole point of the boundary: retransmission is the transaction's problem and
// SIP semantics are the TU's.
//
// Registrar (section 10) is the TU for REGISTER. Proxy (section 16) is the TU for
// everything else. Dialogs (section 12) and a local UA join them in later steps.
//
// A TU's work is async now that the datastore is: it answers a request from a handler
// that runs some time after on_request returned. Holding a strong reference to itself
// across that gap is what keeps it alive, so every TU is shared and can hand out one.
class TransactionUser : public std::enable_shared_from_this<TransactionUser> {
 public:
  virtual ~TransactionUser() = default;

  // transaction is the server transaction the request arrived on. It is null only for a
  // request that travels outside one: the ACK for a 2xx, which is end to end
  // (RFC 3261 17.1.1.3).
  virtual void on_request(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction) = 0;
};

}  // namespace athenasip
