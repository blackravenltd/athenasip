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

#include "../sip_message.h"
#include "transaction_base.h"

namespace athenasip::transactions {

// RFC 3261 17.1.3 and 17.2.3: which transaction a message belongs to.
//
// A transaction is identified by the branch of the topmost Via, the sent-by of that Via
// and the method. Two of the cases are not the obvious one:
//
//   - An ACK for a non-2xx belongs to the INVITE server transaction that sent the final
//     response, not to a transaction of its own. Its CSeq method is ACK, so the key is
//     computed with the method forced to INVITE.
//   - A CANCEL gets its own non-INVITE server transaction and separately names the
//     INVITE transaction it cancels (9.2). Finding that one forces the method to INVITE
//     as well.
//
// RFC 2543 fallback matching, for requests whose branch carries no magic cookie, is
// deferred.
class TransactionMatcher {
 public:
  // branch|sent-by|method. The separators matter: without them "abc" + "INVITE" and
  // "abcI" + "NVITE" are the same string. Empty when the message carries no Via or
  // CSeq, which is not a transaction we can name.
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
  std::unordered_map<std::string, std::shared_ptr<TransactionBase>> _transactions;
};

}  // namespace athenasip::transactions
