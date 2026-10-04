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

std::string verify(const types::Subscriber& subscriber, types::Authorization& credentials, const std::string& method) {
  // RFC 8760: the algorithm the client answered with selects the hash. Absent means MD5
  // (RFC 2617 3.2.1).
  const auto algorithm = Util::to_upper(credentials.contains_field("algorithm") ? credentials.fields["algorithm"] : "MD5");

  if (algorithm != "MD5" && algorithm != "SHA-256") return "an unsupported Digest algorithm " + algorithm;

  const auto& stored = algorithm == "SHA-256" ? subscriber.ha1_sha256 : subscriber.ha1;

  // No stored credential for that algorithm. The caller re-challenges, and the client can
  // answer with the other one.
  if (stored.empty()) return "no " + algorithm + " credential for the subscriber";

  // RFC 2617: response = H(HA1:nonce:HA2), HA2 = H(method:uri). HA1 is what is stored.
  const auto hash = [&algorithm](const std::string& input) { return algorithm == "SHA-256" ? Util::sha256(input) : Util::md5(input); };

  const auto expected = Util::to_lower(hash(stored + ":" + credentials.fields["nonce"] + ":" + hash(method + ":" + credentials.fields["uri"])));

  if (expected != Util::to_lower(credentials.fields["response"])) return "a Digest response that does not match";

  return "";
}

void add_challenges(SIPHeader& header, const std::string& field, const std::string& realm, const std::string& nonce) {
  // SHA-256 first as the preferred algorithm, then MD5, which is all most SIP clients
  // support (RFC 3261 22.4). Both are sent because the subscriber is not yet known.
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
