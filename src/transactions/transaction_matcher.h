//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>

#include "../headers/cseq_header.h"
#include "../headers/via_header.h"
#include "../sip_message.h"
#include "transaction_base.h"

namespace athenasip::transactions {

// RFC 3261 17.1.3 and 17.2.3: which transaction a message belongs to.
//
// A 3261 request is identified by the topmost Via's branch and sent-by, and the method. Two cases force the method
// to INVITE:
//
//   - An ACK for a non-2xx belongs to the INVITE server transaction that sent the final response.
//   - A CANCEL has its own non-INVITE server transaction, and separately names the INVITE transaction it cancels (9.2).
//
// A request whose topmost Via has no branch, or one without the z9hG4bK cookie, is from an RFC 2543 implementation
// and is keyed by 17.2.3's fallback (see `_legacy_key`). Both kinds of key are strings in the one table.
class TransactionMatcher {
 public:
  // branch|sent-by|method, or 2543-<digest>|method for a request with no usable branch. Empty when the message has no
  // Via or CSeq, and for a response without the magic cookie, which can only be matched by its branch.
  static std::string key(const std::shared_ptr<SIPMessage>& message, const std::string& method_override = "");

  void add(const std::string& key, std::shared_ptr<TransactionBase> transaction);
  bool remove(const std::string& key);
  std::shared_ptr<TransactionBase> find(const std::string& key) const;

  // The transaction a request belongs to, or nullptr when the request starts one.
  std::shared_ptr<TransactionBase> match_request(const std::shared_ptr<SIPMessage>& request) const;

  // The INVITE server transaction a CANCEL refers to.
  std::shared_ptr<TransactionBase> match_cancelled(const std::shared_ptr<SIPMessage>& cancel) const;

  // The client transaction a response belongs to (17.1.3): the response's topmost Via is the one this node put on the request.
  std::shared_ptr<TransactionBase> match_response(const std::shared_ptr<SIPMessage>& response) const;

  std::size_t size() const { return _transactions.size(); }

  // Terminates everything and empties the table.
  void terminate_all();

 private:
  static std::string _legacy_key(const std::shared_ptr<SIPMessage>& request, const headers::ViaHeader& via, const headers::CSeqHeader& cseq,
                                 const std::string& method);

  std::unordered_map<std::string, std::shared_ptr<TransactionBase>> _transactions;
};

}  // namespace athenasip::transactions
