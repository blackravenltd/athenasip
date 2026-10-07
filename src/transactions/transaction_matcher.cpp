//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "transaction_matcher.h"

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

#include "../headers/cseq_header.h"
#include "../headers/sip_identity_header.h"
#include "../headers/via_header.h"
#include "../util.h"

namespace athenasip::transactions {

using athenasip::headers::CSeqHeader;
using athenasip::headers::SIPIdentityHeader;
using athenasip::headers::ViaHeader;

namespace {

// RFC 3261 8.1.1.7: every 3261 branch begins with this; a request without it is matched the RFC 2543 way.
bool has_magic_cookie(const std::string& branch) {
  static constexpr char cookie[] = "z9hg4bk";
  static constexpr std::size_t length = sizeof(cookie) - 1;

  if (branch.size() < length) return false;

  for (std::size_t i = 0; i < length; ++i) {
    if (std::tolower(static_cast<unsigned char>(branch[i])) != cookie[i]) return false;
  }

  return true;
}

std::string tag_of(const std::shared_ptr<SIPHeader>& header, const std::string& field) {
  if (!header->contains(field)) return "";

  auto identity = header->headers_map[field][0]->as<SIPIdentityHeader>();
  if (identity == nullptr || identity->value == nullptr) return "";

  const auto tag = identity->value->tags.find("tag");
  return tag == identity->value->tags.end() ? "" : tag->second;
}

// The topmost Via as a string that is the same however the field was parsed: parameters are sorted, because they
// live in an unordered_map.
std::string via_identity(const ViaHeader& via) {
  std::vector<std::string> parameters;
  parameters.reserve(via.parameters.size());
  for (const auto& [name, value] : via.parameters) parameters.push_back(name + "=" + value);
  std::sort(parameters.begin(), parameters.end());

  std::string identity = via.version + " " + via.host;
  for (const auto& parameter : parameters) identity += ";" + parameter;

  return identity;
}

// 128 bits of the digest, enough to name a transaction.
constexpr std::size_t kLegacyKeyLength = 32;

}  // namespace

std::string TransactionMatcher::key(const std::shared_ptr<SIPMessage>& message, const std::string& method_override) {
  if (!message || !message->header) return "";
  if (!message->header->contains("Via") || !message->header->contains("CSeq")) return "";

  auto via = message->header->headers_map["Via"][0]->as<ViaHeader>();
  auto cseq = message->header->headers_map["CSeq"][0]->as<CSeqHeader>();

  if (via == nullptr || cseq == nullptr) return "";

  const auto& method = method_override.empty() ? cseq->method : method_override;
  const auto branch = via->parameters.find("branch");

  if (branch != via->parameters.end() && has_magic_cookie(branch->second)) return branch->second + "|" + via->host + "|" + method;

  // RFC 3261 17.2.3: the fallback needs a Request-URI, so a response whose branch has no cookie matches nothing.
  if (message->header->type != SIPHeader::Type::Request) return "";

  return _legacy_key(message, *via, *cseq, method);
}

// RFC 3261 17.2.3, for a request no 3261 client transaction generated: Request-URI, From tag, Call-ID, CSeq number
// and topmost Via. The To tag is left out, because an ACK carries the response's To tag and the INVITE carried none,
// so a key holding it could never match the two.
//
// The tuple is hashed because the key also names the transaction's log scope and is a level of the MQTT topic
// `Core::transaction_add` publishes on, where '/' and '+' are not allowed. The method stays in the clear for the log.
std::string TransactionMatcher::_legacy_key(const std::shared_ptr<SIPMessage>& request, const ViaHeader& via, const CSeqHeader& cseq,
                                            const std::string& method) {
  const auto& header = request->header;

  std::string material;
  if (header->request_uri) material += header->request_uri->to_string();

  material += "|" + tag_of(header, "From");

  material += "|";
  if (header->contains("Call-ID")) material += header->headers_map["Call-ID"][0]->to_string();

  material += "|" + std::to_string(cseq.sequence);
  material += "|" + via_identity(via);

  return "2543-" + Util::sha256(material).substr(0, kLegacyKeyLength) + "|" + method;
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
