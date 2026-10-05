//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2026 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
// A shared library that is not an AthenaSIP module, as one left in plugins.path by mistake would be.
extern "C" int something_else() { return 42; }
