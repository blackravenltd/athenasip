//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2025 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#include "sip_core.h"

namespace athenasip {

SIPCore::SIPCore() {}
SIPCore::SIPCore(std::shared_ptr<Version> _version, std::shared_ptr<Config> _config, std::shared_ptr<athenasip::loggers::Logger> _logger,
                 std::shared_ptr<athenasip::databases::DB> _db, std::shared_ptr<Registrar> _registrar, std::shared_ptr<Server> _server)
    : version(_version), config(_config), logger(_logger), db(_db), registrar(_registrar), server(_server) {}

}  // namespace athenasip
