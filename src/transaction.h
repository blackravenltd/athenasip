//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

using namespace athenasip::loggers;

namespace athenasip {
class Transaction {
 public:
  std::string id;
  std::shared_ptr<Session> session;
};
}  // namespace athenasip