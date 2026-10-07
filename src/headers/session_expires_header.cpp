//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "session_expires_header.h"

#include "../util.h"

namespace athenasip::headers {

bool SessionExpiresHeader::parse(const std::string& value) {
  delta_seconds = 0;
  refresher.clear();

  const auto text = Util::trim(value);
  if (text.empty()) return false;

  // delta-seconds, then the parameters.
  const auto semicolon = text.find(';');
  const auto seconds = Util::trim(semicolon == std::string::npos ? text : text.substr(0, semicolon));

  if (seconds.empty() || seconds.find_first_not_of("0123456789") != std::string::npos) return false;

  try {
    delta_seconds = static_cast<std::uint32_t>(std::stoul(seconds));
  } catch (const std::exception&) {
    // Too large for 32 bits: unparseable, rather than wrapped to something small.
    delta_seconds = 0;
    return false;
  }

  if (semicolon == std::string::npos) return true;

  std::string rest = text.substr(semicolon + 1);

  while (!rest.empty()) {
    const auto next = rest.find(';');
    const auto parameter = Util::trim(next == std::string::npos ? rest : rest.substr(0, next));
    rest = next == std::string::npos ? "" : rest.substr(next + 1);

    if (parameter.empty()) continue;

    const auto equals = parameter.find('=');
    if (equals == std::string::npos) continue;

    // Only refresher is read; the rest are generic-param.
    if (Util::to_lower(Util::trim(parameter.substr(0, equals))) == "refresher") {
      refresher = Util::to_lower(Util::trim(parameter.substr(equals + 1)));
    }
  }

  return true;
}

std::string SessionExpiresHeader::to_string() const {
  std::string out = std::to_string(delta_seconds);
  if (!refresher.empty()) out += ";refresher=" + refresher;
  return out;
}

}  // namespace athenasip::headers
