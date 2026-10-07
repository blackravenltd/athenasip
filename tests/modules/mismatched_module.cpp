//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// A module built against another contract version, which the server must refuse without calling it.
#include <cstdint>
#include <cstdlib>

#include "plugins/plugin_module.h"

extern "C" ATHENASIP_MODULE_EXPORT std::uint32_t athenasip_module_api_version() { return athenasip::plugins::API_VERSION + 1; }
extern "C" ATHENASIP_MODULE_EXPORT const char* athenasip_module_name() { return "mismatched"; }
extern "C" ATHENASIP_MODULE_EXPORT void athenasip_module_register(athenasip::plugins::ModuleHost*) { std::abort(); }
