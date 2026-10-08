-- Emergency numbers always go out, by the trunk whose attributes say {"emergency": true}, whatever else the
-- policy would do with them, and never wait on a ring timeout.
--
-- policy.lua.entry: emergency.lua

local a = athenasip
local trunks = require "athenasip.trunks"

local EMERGENCY = {["999"] = true, ["112"] = true, ["911"] = true}

authorize, on_failure, register = trunks.authorize, trunks.on_failure, trunks.register

function route(request)
  local dialled = request.uri.user
  if dialled and EMERGENCY[dialled] and not request.trunk then
    for _, trunk in ipairs(a.store.trunks()) do
      if trunk.attributes.emergency then
        if trunk.attributes.caller_id then request:set_from{user = trunk.attributes.caller_id} end
        return a.route.forward({a.route.trunk(trunk, {user = dialled})}, {media = {media_anchor = true, media_profile = "rtp"}})
      end
    end
    a.log.error("A call to " .. dialled .. " and no trunk is marked emergency")
  end
  return trunks.route(request)
end
