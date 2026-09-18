//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "transaction_matcher.h"

#include <utility>
#include <vector>

#include "../headers/cseq_header.h"
#include "../headers/via_header.h"

namespace athenasip::transactions {

using athenasip::headers::CSeqHeader;
using athenasip::headers::ViaHeader;

std::string TransactionMatcher::key(const std::shared_ptr<SIPMessage>& message, const std::string& method_override) {
  if (!message || !message->header) return "";
  if (!message->header->contains("Via") || !message->header->contains("CSeq")) return "";

  auto via = message->header->headers_map["Via"][0]->as<ViaHeader>();
  auto cseq = message->header->headers_map["CSeq"][0]->as<CSeqHeader>();

  if (via == nullptr || cseq == nullptr) return "";

  const auto branch = via->parameters.find("branch");
  if (branch == via->parameters.end() || branch->second.empty()) return "";

  return branch->second + "|" + via->host + "|" + (method_override.empty() ? cseq->method : method_override);
}

void TransactionMatcher::add(const std::string& key, std::shared_ptr<TransactionBase> transaction) {
  if (key.empty() || !transaction) return;
  _transactions[key] = std::move(transaction);
}

bool TransactionMatcher::remove(const std::string& key) { return _transactions.erase(key) > 0; }

std::shared_ptr<TransactionBase> TransactionMatcher::find(const std::string& key) const {
  if (key.empty()) return nullptr;

  auto search = _transactions.find(key);
  if (search == _transactions.end()) return nullptr;
  return search->second;
}

std::shared_ptr<TransactionBase> TransactionMatcher::match_request(const std::shared_ptr<SIPMessage>& request) const {
  if (!request || !request->header) return nullptr;

  const bool is_ack = request->header->request_method == "ACK";
  return find(key(request, is_ack ? "INVITE" : ""));
}

std::shared_ptr<TransactionBase> TransactionMatcher::match_cancelled(const std::shared_ptr<SIPMessage>& cancel) const { return find(key(cancel, "INVITE")); }

std::shared_ptr<TransactionBase> TransactionMatcher::match_response(const std::shared_ptr<SIPMessage>& response) const { return find(key(response)); }

void TransactionMatcher::terminate_all() {
  std::vector<std::shared_ptr<TransactionBase>> transactions;
  transactions.reserve(_transactions.size());
  for (const auto& [key, transaction] : _transactions) transactions.push_back(transaction);
  _transactions.clear();

  for (const auto& transaction : transactions) transaction->terminate();
}

}  // namespace athenasip::transactions
