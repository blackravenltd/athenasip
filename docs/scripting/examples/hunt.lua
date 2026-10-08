-- A hunt group: a call to sales rings each member for 15 seconds in turn, then the sales mobile through the
-- trunk. Every other call is the trunks module's.
--
-- policy.lua.entry: hunt.lua

local a = athenasip
local trunks = require "athenasip.trunks"

local SALES = {"sip:anna@example.com", "sip:ben@example.com", "sip:chris@example.com"}
local SALES_MOBILE = "+447700900123"

authorize, on_failure, register = trunks.authorize, trunks.on_failure, trunks.register

function route(request)
  if request.uri.user ~= "sales" then return trunks.route(request) end

  local targets = {}
  for _, aor in ipairs(SALES) do
    local member = a.store.subscriber(aor)
    if member then targets[#targets + 1] = a.route.subscriber(member, {ring_timeout = 15}) end
  end

  local mobile = trunks.for_number(SALES_MOBILE)[1]
  if mobile then targets[#targets + 1] = a.route.trunk(mobile.trunk, {user = mobile.number}) end

  return a.route.forward(targets, {media = a.config.behaviour})
end
