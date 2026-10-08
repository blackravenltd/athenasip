-- A blocklist: calls from these numbers, or matching these patterns, are declined before anything else is
-- decided. Lua patterns, so "%+4490" is a prefix and "^anonymous$" a whole user part.
--
-- policy.lua.entry: blocklist.lua

local a = athenasip
local trunks = require "athenasip.trunks"

local BLOCKED = {"^%+4490", "^%+447700900999$", "^anonymous$"}

route, on_failure, register = trunks.route, trunks.on_failure, trunks.register

function authorize(request)
  local caller = request.from and request.from.uri.user
  if caller and not request.in_dialog then
    for _, pattern in ipairs(BLOCKED) do
      if caller:match(pattern) then
        a.log.info("Declining " .. caller .. ", blocked by " .. pattern)
        return a.auth.reject(603, "Decline")
      end
    end
  end
  return trunks.authorize(request)
end
