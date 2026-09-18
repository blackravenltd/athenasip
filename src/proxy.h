//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "loggers/logger.h"
#include "sip_message.h"
#include "transaction_user.h"
#include "transactions/transaction_base.h"
#include "types/location.h"

namespace athenasip {

class Channel;
class Core;

// RFC 3261 section 16: the proxy, and the transaction user for everything that is not a
// REGISTER.
//
// This step covers target determination (16.5) and forwarding (16.6) with serial
// forking, plus response processing far enough to pass a response back up the server
// transaction (16.7). Route and Record-Route handling, loop detection and best-response
// selection across parallel branches come with the rest of section 16.
class Proxy : public TransactionUser {
 public:
  Proxy(std::shared_ptr<loggers::Logger> logger, std::shared_ptr<Core> core);

  void on_request(std::shared_ptr<SIPMessage> request, std::shared_ptr<transactions::TransactionBase> transaction) override;

  // RFC 3261 9.2: a CANCEL is answered 200 OK on its own transaction, and the INVITE
  // server transaction it names is answered 487. Forwarding the CANCEL down the branches
  // already tried (16.10) waits for the rest of section 16.
  void on_cancel(std::shared_ptr<SIPMessage> cancel, std::shared_ptr<transactions::TransactionBase> cancel_transaction,
                 std::shared_ptr<transactions::TransactionBase> invite_transaction);

 private:
  // One request being proxied: the server transaction it arrived on, the targets left to
  // try, and the best response seen so far. It is kept alive by the callbacks the client
  // transactions hold, and dies with the last of them.
  struct Context {
    std::shared_ptr<SIPMessage> request;
    std::shared_ptr<transactions::TransactionBase> server;
    std::weak_ptr<Channel> outbound;

    std::vector<types::Location> targets;
    std::size_t next = 0;

    std::shared_ptr<SIPMessage> best;
  };

  // Rewrites the request for one hop: Request-URI, Max-Forwards, and this node's Via
  // with a fresh branch (RFC 3261 16.6 steps 2, 3 and 8). False when Max-Forwards has
  // run out.
  bool _prepare_forward(const std::shared_ptr<SIPMessage>& request, const std::shared_ptr<Channel>& channel, const std::shared_ptr<SIPUri>& target) const;

  // Sends to the next untried target, and answers the caller when there are none left.
  void _forward_next(const std::shared_ptr<Context>& context);

  void _on_response(const std::shared_ptr<Context>& context, const std::shared_ptr<SIPMessage>& response);

  void _send_status(const std::shared_ptr<transactions::TransactionBase>& transaction, const std::shared_ptr<SIPMessage>& request, std::uint16_t code,
                    const std::string& reason);

  std::shared_ptr<loggers::Logger> _logger;
  std::weak_ptr<Core> _core;
};

}  // namespace athenasip
