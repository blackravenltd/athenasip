--
-- AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
--
-- Copyright (C) 2026 Tom Cully <mail@tomcully.com>
-- Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
--
-- The entry script when policy.lua.path holds no main.lua of its own. It behaves exactly as builtin:// does.
--
-- To change one decision, copy this file into a directory on policy.lua.path and replace that hook, calling
-- the standard one for everything else:
--
--   local std = require "athenasip.standard"
--   authorize, on_failure, register = std.authorize, std.on_failure, std.register
--   function route(request)
--     if request.uri.user == "reception" then
--       return athenasip.route.forward({athenasip.route.uri("sip:desk@192.0.2.40")})
--     end
--     return std.route(request)
--   end

local std = require "athenasip.standard"

authorize, route, on_failure, register = std.authorize, std.route, std.on_failure, std.register
