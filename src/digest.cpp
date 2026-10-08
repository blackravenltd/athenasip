//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "digest.h"

#include <cstdio>

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

  // RFC 2617: response = H(HA1:nonce:HA2), HA2 = H(method:uri). HA1 is what is stored. With qop=auth (RFC 2617
  // 3.2.2.1, RFC 7616 3.4.1) the client's nc, cnonce and qop go between the nonce and HA2; auth-int, which would
  // need the body, is not offered.
  const auto hash = [&algorithm](const std::string& input) { return algorithm == "SHA-256" ? Util::sha256(input) : Util::md5(input); };
  const auto ha2 = hash(method + ":" + credentials.fields["uri"]);

  std::string expected;
  if (credentials.contains_field("qop")) {
    if (Util::to_lower(credentials.fields["qop"]) != "auth") return "an unsupported Digest qop " + credentials.fields["qop"];
    expected = hash(stored + ":" + credentials.fields["nonce"] + ":" + credentials.fields["nc"] + ":" + credentials.fields["cnonce"] + ":auth:" + ha2);
  } else {
    expected = hash(stored + ":" + credentials.fields["nonce"] + ":" + ha2);
  }
  expected = Util::to_lower(expected);

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

namespace {

bool known_algorithm(const std::string& algorithm) {
  return algorithm == "MD5" || algorithm == "MD5-SESS" || algorithm == "SHA-256" || algorithm == "SHA-256-SESS";
}

// The qop values a challenge offers: a quoted, comma-separated list (RFC 7616 3.3).
bool offers(const std::string& options, const std::string& wanted) {
  std::size_t start = 0;
  while (start <= options.size()) {
    const auto comma = options.find(',', start);
    auto option = Util::to_lower(options.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
    option.erase(0, option.find_first_not_of(" \t"));
    option.erase(option.find_last_not_of(" \t") + 1);
    if (option == wanted) return true;
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return false;
}

}  // namespace

std::optional<types::Authorization> respond(const types::Authorization& challenge, const std::string& username, const std::string& password,
                                            const std::string& method, const std::string& uri, const std::string& cnonce, std::uint32_t nc) {
  if (Util::to_lower(challenge.type) != "digest") return std::nullopt;

  const auto field = [&challenge](const std::string& name) -> std::string {
    const auto found = challenge.fields.find(name);
    return found == challenge.fields.end() ? std::string() : found->second;
  };

  // Absent means MD5 (RFC 2617 3.2.1).
  const auto written = field("algorithm").empty() ? std::string("MD5") : field("algorithm");
  const auto algorithm = Util::to_upper(written);
  if (!known_algorithm(algorithm)) return std::nullopt;

  const bool sha256 = algorithm.rfind("SHA-256", 0) == 0;
  const bool session = algorithm.size() > 5 && algorithm.substr(algorithm.size() - 5) == "-SESS";
  const auto hash = [sha256](const std::string& input) { return Util::to_lower(sha256 ? Util::sha256(input) : Util::md5(input)); };

  const auto realm = field("realm");
  const auto nonce = field("nonce");
  const auto options = field("qop");
  if (!options.empty() && !offers(options, "auth")) return std::nullopt;
  const bool qop = !options.empty();

  // RFC 7616 3.4.2: -sess hashes the nonces into HA1.
  auto ha1 = hash(username + ":" + realm + ":" + password);
  if (session) ha1 = hash(ha1 + ":" + nonce + ":" + cnonce);

  const auto ha2 = hash(method + ":" + uri);

  char count[9];
  std::snprintf(count, sizeof(count), "%08x", nc);

  types::Authorization out;
  out.type = "Digest";
  out.fields["username"] = username;
  out.fields["realm"] = realm;
  out.fields["nonce"] = nonce;
  out.fields["uri"] = uri;
  if (!field("algorithm").empty()) out.fields["algorithm"] = written;

  if (qop) {
    out.fields["qop"] = "auth";
    out.fields["nc"] = count;
    out.fields["cnonce"] = cnonce;
    out.fields["response"] = hash(ha1 + ":" + nonce + ":" + count + ":" + cnonce + ":auth:" + ha2);
  } else {
    out.fields["response"] = hash(ha1 + ":" + nonce + ":" + ha2);
  }

  // RFC 7616 3.4: the opaque comes back unchanged.
  if (!field("opaque").empty()) out.fields["opaque"] = field("opaque");
  return out;
}

std::optional<types::Authorization> preferred(const std::vector<types::Authorization>& challenges) {
  const types::Authorization* md5 = nullptr;
  for (const auto& challenge : challenges) {
    if (Util::to_lower(challenge.type) != "digest") continue;

    const auto found = challenge.fields.find("algorithm");
    const auto algorithm = Util::to_upper(found == challenge.fields.end() ? std::string("MD5") : found->second);
    if (algorithm.rfind("SHA-256", 0) == 0 && known_algorithm(algorithm)) return challenge;
    if (md5 == nullptr && known_algorithm(algorithm)) md5 = &challenge;
  }
  if (md5) return *md5;
  return std::nullopt;
}

}  // namespace athenasip::digest
