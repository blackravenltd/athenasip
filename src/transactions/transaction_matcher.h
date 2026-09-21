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
// A request from a 3261 implementation is identified by the branch of the topmost Via,
// the sent-by of that Via and the method. Two of the cases are not the obvious one:
//
//   - An ACK for a non-2xx belongs to the INVITE server transaction that sent the final
//     response, not to a transaction of its own. Its CSeq method is ACK, so the key is
//     computed with the method forced to INVITE.
//   - A CANCEL gets its own non-INVITE server transaction and separately names the
//     INVITE transaction it cancels (9.2). Finding that one forces the method to INVITE
//     as well.
//
// A request whose topmost Via carries no branch, or one that does not begin with the
// z9hG4bK magic cookie, came from an RFC 2543 implementation, which had no transaction
// identifier to offer. 17.2.3's fallback names it by the fields that were the same
// across a retransmission then: Request-URI, From tag, Call-ID, CSeq number, topmost
// Via and method, hashed into one token for the reasons at `_legacy_key`. Both forms
// are strings in the one table, so nothing above this layer has to know which kind it
// is holding.
class TransactionMatcher {
 public:
  // branch|sent-by|method, or 2543-<digest>|method for a request with no usable branch. The
  // separators matter: without them "abc" + "INVITE" and "abcI" + "NVITE" are the same
  // string. Empty when the message carries no Via or CSeq, which is not a transaction
  // we can name, and for a response with no magic cookie: 17.2.3's fallback includes
  // the Request-URI, which a response does not have, so a response can only ever be
  // matched by its branch.
  static std::string key(const std::shared_ptr<SIPMessage>& message, const std::string& method_override = "");

  void add(const std::string& key, std::shared_ptr<TransactionBase> transaction);
  bool remove(const std::string& key);
  std::shared_ptr<TransactionBase> find(const std::string& key) const;

  // The transaction a request belongs to, or nullptr when the request starts one.
  std::shared_ptr<TransactionBase> match_request(const std::shared_ptr<SIPMessage>& request) const;

  // The INVITE server transaction a CANCEL refers to.
  std::shared_ptr<TransactionBase> match_cancelled(const std::shared_ptr<SIPMessage>& cancel) const;

  // The client transaction a response belongs to (17.1.3). The topmost Via of a
  // response is the one this node put on the request, so the key is the same one the
  // client transaction was filed under.
  std::shared_ptr<TransactionBase> match_response(const std::shared_ptr<SIPMessage>& response) const;

  std::size_t size() const { return _transactions.size(); }

  // Terminates everything and empties the table. terminate() fires on_terminated, which
  // removes from the table being walked, so take a copy and clear first.
  void terminate_all();

 private:
  static std::string _legacy_key(const std::shared_ptr<SIPMessage>& request, const headers::ViaHeader& via, const headers::CSeqHeader& cseq,
                                 const std::string& method);

  std::unordered_map<std::string, std::shared_ptr<TransactionBase>> _transactions;
};

}  // namespace athenasip::transactions
