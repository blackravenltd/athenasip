-- Following a redirect. The node does not recurse on a 3xx (RFC 3261 16.7 leaves it to the proxy): the 3xx
-- competes as the best failure. This tries the Contacts a 3xx names, once per call.
--
-- policy.lua.entry: follow-redirect.lua

local a = athenasip
local std = require "athenasip.standard"

authorize, route, register = std.authorize, std.route, std.register

-- The calls already redirected, so a 3xx loop ends. Kept small: a script's state lives as long as the scripts.
local followed, count = {}, 0

function on_failure(request, response, state)
  local call = request.call_id
  if response.code >= 300 and response.code < 400 and call and not followed[call] then
    if count >= 1000 then followed, count = {}, 0 end
    followed[call], count = true, count + 1
    local targets = {}
    for _, contact in ipairs(response:headers("Contact")) do
      local uri = contact:match("<([^>]+)>") or contact
      targets[#targets + 1] = a.route.uri(uri)
    end
    if #targets > 0 then return targets end
  end
  return std.on_failure(request, response, state)
end
