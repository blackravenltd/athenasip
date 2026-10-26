//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "digest.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <stdexcept>

#include "util.h"

namespace athenasip::digest {

bool is_complete(const std::shared_ptr<types::Authorization>& credentials) {
  return credentials && credentials->type == "Digest" && credentials->contains_field("realm") && credentials->contains_field("nonce") &&
         credentials->contains_field("response") && credentials->contains_field("uri");
}

std::string verify(const types::Subscriber& subscriber, types::Authorization& credentials, const std::string& method, const std::string& request_uri) {
  // RFC 3261 22.4 (5): the uri the client hashed is the Request-URI it sent, so an answer captured from one
  // request cannot sign another.
  if (credentials.fields["uri"] != request_uri) return "a Digest uri that is not the Request-URI";

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

  // In constant time: how long a comparison takes must not say how much of the answer was right.
  const auto given = Util::to_lower(credentials.fields["response"]);
  if (given.size() != expected.size() || CRYPTO_memcmp(given.data(), expected.data(), expected.size()) != 0) {
    return "a Digest response that does not match";
  }

  return "";
}

void add_challenges(SIPHeader& header, const std::string& field, const std::string& realm, const std::string& nonce, bool stale) {
  // SHA-256 first as the preferred algorithm, then MD5, which is all most SIP clients
  // support (RFC 3261 22.4). Both are sent because the subscriber is not yet known. MD5
  // must stay last and in a field of its own: JsSIP keeps only the last field of a header
  // and refuses any algorithm but MD5, so AthenaPhone and the console would stop
  // registering if it were first or joined into one value with SHA-256.
  for (const auto& algorithm : {std::string("SHA-256"), std::string("MD5")}) {
    types::Authorization challenge;
    challenge.type = "Digest";
    challenge.fields["realm"] = realm;
    challenge.fields["nonce"] = nonce;
    challenge.fields["algorithm"] = algorithm;
    if (stale) challenge.fields["stale"] = "true";
    header.add(field, challenge.to_string());
  }
}

namespace {
// The HMAC of the nonce's first three parts and the address it was sent to, hex.
std::string nonce_signature(const types::Realm& realm, const std::string& signed_part, const std::string& source) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
  unsigned int length = 0;
  const auto input = signed_part + "|" + source;
  HMAC(EVP_sha256(), realm.nonce_secret.data(), static_cast<int>(realm.nonce_secret.size()), reinterpret_cast<const unsigned char*>(input.data()),
       input.size(), mac.data(), &length);
  return Util::to_hex(mac.data(), static_cast<uint16_t>(length));
}
}  // namespace

std::string nonce_mint(const types::Realm& realm, const std::string& source, std::time_t now) {
  std::array<unsigned char, 16> random{};
  if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) throw std::runtime_error("RAND_bytes failed");

  const auto signed_part = std::to_string(realm.id) + ":" + Util::to_hex(random.data(), random.size()) + ":" + std::to_string(now);
  return signed_part + ":" + nonce_signature(realm, signed_part, source);
}

NonceState nonce_state(const types::Realm& realm, const std::string& nonce, const std::string& source, std::time_t now) {
  // realm:random:minted:signature. The colons are not in any part, so the last one ends the signed part.
  const auto last = nonce.rfind(':');
  if (last == std::string::npos || last == 0) return NonceState::bad;
  const auto signed_part = nonce.substr(0, last);
  const auto signature = nonce.substr(last + 1);

  // The realm's own, under its secret, compared in constant time.
  const auto expected = nonce_signature(realm, signed_part, source);
  if (signature.size() != expected.size() || CRYPTO_memcmp(signature.data(), expected.data(), expected.size()) != 0) return NonceState::bad;

  const auto first = signed_part.find(':');
  const auto second = signed_part.find(':', first == std::string::npos ? std::string::npos : first + 1);
  if (first == std::string::npos || second == std::string::npos) return NonceState::bad;
  if (signed_part.substr(0, first) != std::to_string(realm.id)) return NonceState::bad;

  const auto minted_text = signed_part.substr(second + 1);
  std::int64_t minted = 0;
  const auto parsed = std::from_chars(minted_text.data(), minted_text.data() + minted_text.size(), minted);
  if (parsed.ec != std::errc() || parsed.ptr != minted_text.data() + minted_text.size()) return NonceState::bad;

  // From the future is not ours either: a clock cannot have signed what it has not reached, bar skew between
  // nodes, which a minute covers.
  if (minted > static_cast<std::int64_t>(now) + 60) return NonceState::bad;
  if (static_cast<std::int64_t>(now) - minted > static_cast<std::int64_t>(realm.nonce_expiry)) return NonceState::stale;
  return NonceState::good;
}

NonceCounts::NonceCounts(std::size_t capacity) : _capacity(capacity) {}

bool NonceCounts::accept(const types::Authorization& credentials, std::time_t expires_at, std::time_t now) {
  const auto field = [&credentials](const std::string& name) -> std::string {
    const auto found = credentials.fields.find(name);
    return found == credentials.fields.end() ? std::string() : found->second;
  };
  const auto nonce = field("nonce");
  if (nonce.empty()) return false;

  // RFC 7616 3.4: nc is eight hex digits. Without qop there is no count, and the nonce is good for one answer.
  std::uint32_t nc = 0;
  if (!field("qop").empty()) {
    const auto text = field("nc");
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), nc, 16);
    if (text.empty() || text.size() > 8 || parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || nc == 0) return false;
  }

  std::lock_guard<std::mutex> lock(_mutex);

  // Forgotten with the nonce. Full means a flood of distinct nonces; the oldest answers go, and with them the
  // guard against a replay of those, which the nonce lifetime bounds.
  if (_seen.size() >= _capacity) {
    for (auto it = _seen.begin(); it != _seen.end();) it = it->second.expires_at <= now ? _seen.erase(it) : std::next(it);
    while (_seen.size() >= _capacity) _seen.erase(_seen.begin());
  }

  const auto found = _seen.find(nonce);
  if (found == _seen.end()) {
    _seen.emplace(nonce, Seen{nc, expires_at});
    return true;
  }
  if (found->second.expires_at <= now) {
    found->second = Seen{nc, expires_at};
    return true;
  }
  if (nc <= found->second.last_nc) return false;
  found->second.last_nc = nc;
  return true;
}

std::size_t NonceCounts::size() {
  std::lock_guard<std::mutex> lock(_mutex);
  return _seen.size();
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
