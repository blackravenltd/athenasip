//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "digest.h"

#include "util.h"

namespace athenasip::digest {

bool is_complete(const std::shared_ptr<types::Authorization>& credentials) {
  return credentials && credentials->type == "Digest" && credentials->contains_field("realm") && credentials->contains_field("nonce") &&
         credentials->contains_field("response") && credentials->contains_field("uri");
}

std::string verify(const types::Account& account, types::Authorization& credentials, const std::string& method) {
  // RFC 8760: the client answered one of the challenges, and which one it answered says
  // which hash to check with. Absent means MD5, which is what RFC 2617 3.2.1 says and
  // what every client that has never heard of anything else sends.
  const auto algorithm = Util::to_upper(credentials.contains_field("algorithm") ? credentials.fields["algorithm"] : "MD5");

  if (algorithm != "MD5" && algorithm != "SHA-256") return "an unsupported Digest algorithm " + algorithm;

  const auto& stored = algorithm == "SHA-256" ? account.ha1_sha256 : account.ha1;

  // An account with no credential for the algorithm it answered with cannot be checked.
  // Challenging again is the honest answer: the next challenge carries both algorithms
  // and the client can come back with the other one.
  if (stored.empty()) return "no " + algorithm + " credential for the account";

  // RFC 2617: HA1 is stored, so the check is HA1:nonce:HA2 with HA2 over method and URI.
  const auto hash = [&algorithm](const std::string& input) { return algorithm == "SHA-256" ? Util::sha256(input) : Util::md5(input); };

  const auto expected = Util::to_lower(hash(stored + ":" + credentials.fields["nonce"] + ":" + hash(method + ":" + credentials.fields["uri"])));

  if (expected != Util::to_lower(credentials.fields["response"])) return "a Digest response that does not match";

  return "";
}

void add_challenges(SIPHeader& header, const std::string& field, const std::string& realm, const std::string& nonce) {
  // SHA-256 leads because a client that can do better than MD5 should, and MD5 follows
  // because nearly every SIP client can do nothing else (RFC 3261 22.4 knows only MD5).
  //
  // A challenge is sent before this node knows which account is answering, so both go
  // out every time. An account with no SHA-256 credential - one imported as a bare MD5
  // hash - is re-challenged for MD5 alone when it answers with SHA-256.
  for (const auto& algorithm : {std::string("SHA-256"), std::string("MD5")}) {
    types::Authorization challenge;
    challenge.type = "Digest";
    challenge.fields["realm"] = realm;
    challenge.fields["nonce"] = nonce;
    challenge.fields["algorithm"] = algorithm;

    header.add(field, challenge.to_string());
  }
}

}  // namespace athenasip::digest
