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

// RFC 3261 8.1.1.7: every branch a 3261 implementation generates begins with this, so
// its absence is what identifies a request that has to be matched the 2543 way.
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

// The whole topmost Via, as a string that is the same every time the same field is
// parsed. The parameters live in an unordered_map, so they are sorted rather than
// walked in whatever order the container hands them over.
std::string via_identity(const ViaHeader& via) {
  std::vector<std::string> parameters;
  parameters.reserve(via.parameters.size());
  for (const auto& [name, value] : via.parameters) parameters.push_back(name + "=" + value);
  std::sort(parameters.begin(), parameters.end());

  std::string identity = via.version + " " + via.host;
  for (const auto& parameter : parameters) identity += ";" + parameter;

  return identity;
}

// Enough of the digest to name a transaction: 128 bits, on a path that only a pre-3261
// endpoint reaches.
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

  // A response has no Request-URI, and 17.2.3 says in as many words that the fallback
  // tuple cannot match one. A response whose branch is not ours belongs to no client
  // transaction here, which is what an empty key says.
  if (message->header->type != SIPHeader::Type::Request) return "";

  return _legacy_key(message, *via, *cseq, method);
}

// RFC 3261 17.2.3, the procedures for a request that no 3261 client transaction
// generated: Request-URI, To tag, From tag, Call-ID, CSeq and topmost Via.
//
// The To tag is the one field left out, because including it would break the case it is
// there for. An ACK carries the To tag of the response, and the INVITE that created the
// transaction carried none, so a key holding the To tag could never match the two to
// each other. Its purpose in the rule is to keep an ACK for a 2xx away from the server
// transaction, and that is already true here without it: a 2xx terminates the INVITE
// server transaction the moment it is sent (17.2.1), so by the time its ACK arrives
// there is nothing in the table for it to match and it reaches the TU, which is where
// an end-to-end ACK belongs.
//
// The tuple is hashed rather than carried, because the key is not only a map key: it
// names the transaction in its log scope and it is a level of the MQTT topic
// `Core::transaction_add` publishes on. A Request-URI and a Via would put '/' - the
// topic separator - inside one level, and a URI holding a telephone number would put
// '+' in a topic name, which MQTT does not allow at all. The method stays in the clear
// so a log line still says what the transaction is.
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
