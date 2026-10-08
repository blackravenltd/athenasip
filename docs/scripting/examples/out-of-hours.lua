-- Out of hours: a call in from a carrier after six in the evening, before nine in the morning or at the weekend
-- goes to the on-call mobile, out through the best trunk for it. In hours, the trunks module decides.
--
-- policy.lua.entry: out-of-hours.lua

local a = athenasip
local trunks = require "athenasip.trunks"

local ON_CALL = "+447700900456"

authorize, on_failure, register = trunks.authorize, trunks.on_failure, trunks.register

local function out_of_hours()
  local now = os.date("*t")
  local weekend = now.wday == 1 or now.wday == 7
  return weekend or now.hour < 9 or now.hour >= 18
end

function route(request)
  if request.trunk and out_of_hours() then
    local best = trunks.for_number(ON_CALL)[1]
    if best then
      a.log.info("Out of hours: " .. tostring(request.to and request.to.uri.user) .. " to the on-call mobile")
      return a.route.forward({a.route.trunk(best.trunk, {user = best.number})}, {media = a.config.behaviour})
    end
  end
  return trunks.route(request)
end
