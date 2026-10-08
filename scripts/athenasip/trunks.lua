--
-- AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
--
-- Copyright (C) 2026 Tom Cully <mail@tomcully.com>
-- Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
--
-- Trunks without writing Lua: everything here is driven by the trunks kept over the API and their attributes.
-- Use it as the policy with a main.lua of one line:
--
--   authorize, route, on_failure, register = require("athenasip.trunks").hooks()
--
-- A trunk's attributes it reads, all optional:
--
--   prefixes      numbers it carries, in E.164 ("+44", "+1"); the longest match wins, then the lowest priority
--   priority      among trunks with the same match, lower first (default 100)
--   country       country code, for reading a national number dialled ("020 7123 4567" in "44")
--   dial_format   "e164" (default, +442071234567) or "digits" (442071234567), as the carrier wants the number
--   caller_id     the number presented when calling out, in E.164
--   numbers       the numbers it brings in, each to an address of record: {"+442071234567": "sip:alice@example.com"}
--   default       where a number in no list goes; without it, such a call is refused
--
-- Everything else is athenasip.standard's: subscribers calling each other, and registrations.

local a = athenasip
local std = require "athenasip.standard"

local trunks = {}

-- Digits with a leading + for an international number, from whatever was dialled: "+44 (0)20 7123 4567",
-- "00442071234567", or "020 7123 4567" with the country given.
function trunks.e164(number, country)
  if not number then return nil end
  local plus = number:match("^%s*%+") ~= nil
  local digits = number:gsub("%(0%)", ""):gsub("[^%d]", "")
  if digits == "" then return nil end
  if plus then return "+" .. digits end
  if digits:sub(1, 2) == "00" then return "+" .. digits:sub(3) end
  if country and digits:sub(1, 1) == "0" then return "+" .. country .. digits:sub(2) end
  return digits
end

-- Whether a user part is a number someone dialled rather than a name.
function trunks.is_number(user)
  return user ~= nil and user:match("^%+?[%d%s%-%.%(%)]+$") ~= nil and user:match("%d") ~= nil
end

-- The trunks that carry a number, best first, each with the number as that trunk reads it.
function trunks.for_number(dialled)
  local found = {}
  for _, trunk in ipairs(a.store.trunks()) do
    local attributes = trunk.attributes
    local number = trunks.e164(dialled, attributes.country)
    local longest = -1
    for _, prefix in ipairs(attributes.prefixes or {}) do
      if number and number:sub(1, #prefix) == prefix and #prefix > longest then longest = #prefix end
    end
    if longest >= 0 then
      found[#found + 1] = {trunk = trunk, number = number, longest = longest, priority = attributes.priority or 100}
    end
  end

  table.sort(found, function(x, y)
    if x.longest ~= y.longest then return x.longest > y.longest end
    if x.priority ~= y.priority then return x.priority < y.priority end
    return x.trunk.name < y.trunk.name
  end)
  return found
end

-- The trunk a request came from, by the address it came from.
function trunks.from(request)
  local source = request.source
  if not source then return nil end
  for _, trunk in ipairs(a.store.trunks()) do
    if trunk:admits(source.address) then return trunk end
  end
end

local function as_dialled(trunk, number)
  if trunk.attributes.dial_format == "digits" then return (number:gsub("^%+", "")) end
  return number
end

-- A request from a trunk's addresses is the trunk's; anything else is as standard.
function trunks.authorize(request)
  local starts_something = not request.in_dialog and not request.from_peer and not request.has_flow_token
  if starts_something and request.method ~= "ACK" and request.method ~= "CANCEL" and request.method ~= "REGISTER" then
    local trunk = trunks.from(request)
    if trunk then return a.auth.trusted{trunk = trunk.name} end
  end
  return std.authorize(request)
end

-- A call in from a carrier: the number dialled names a subscriber.
function trunks.inbound(request)
  local trunk = a.store.trunk(request.trunk)
  if not trunk then return a.route.reply(404, "Not Found") end

  -- A carrier we register to sends to the Contact we registered, and puts the number dialled in the To.
  local dialled = request.uri.user
  if (dialled == nil or dialled == trunk.contact_user) and request.to then dialled = request.to.uri.user end

  local number = trunks.e164(dialled, trunk.attributes.country)
  local numbers = trunk.attributes.numbers or {}
  local aor = (number and numbers[number]) or (dialled and numbers[dialled]) or trunk.attributes.default
  if not aor then
    a.log.info("Trunk " .. trunk.name .. " brought in " .. tostring(dialled) .. ", which goes nowhere - 404")
    return a.route.reply(404, "Not Found")
  end

  local subscriber = a.store.subscriber(aor)
  if not subscriber then
    a.log.warn("Trunk " .. trunk.name .. " maps " .. tostring(number) .. " to " .. aor .. ", which is no subscriber - 404")
    return a.route.reply(404, "Not Found")
  end

  local bindings = a.store.locations(subscriber)
  if #bindings == 0 then return a.route.reply(480, "Temporarily Unavailable") end

  local realm = a.store.realm(subscriber.realm)
  local behaviour = realm and realm.behaviour_effective or a.config.behaviour
  return a.route.forward({a.route.subscriber(subscriber, {bindings = bindings})}, {media = behaviour, rewrite_contact = behaviour.rewrite_contact})
end

-- A subscriber dialling a number no subscriber has: out by the best trunk, the others after it.
function trunks.outbound(request, realm)
  local candidates = trunks.for_number(request.uri.user)
  if #candidates == 0 then
    a.log.info("No trunk carries " .. request.uri.user .. " - 404")
    return a.route.reply(404, "Not Found")
  end

  local targets = {}
  for _, candidate in ipairs(candidates) do
    targets[#targets + 1] = a.route.trunk(candidate.trunk, {user = as_dialled(candidate.trunk, candidate.number)})
  end

  -- The first trunk's caller ID, asserted to the carrier (RFC 3325 9.1).
  local first = candidates[1].trunk
  local caller_id = first.attributes.caller_id
  if caller_id then
    request:set_from{user = as_dialled(first, caller_id)}
    request:set_header("P-Asserted-Identity", "<sip:" .. as_dialled(first, caller_id) .. "@" .. first.uri.host .. ">")
  end

  local behaviour = realm.behaviour_effective
  return a.route.forward(targets, {media = behaviour, rewrite_contact = behaviour.rewrite_contact})
end

function trunks.route(request)
  if request.trunk then return trunks.inbound(request) end

  -- Only a caller in one of this node's realms, which authorize has already made prove itself, calls out.
  local from = request.from
  local realm = from and a.store.realm(from.uri.host:lower())
  if realm and trunks.is_number(request.uri.user) and not a.store.subscriber(request.uri) then return trunks.outbound(request, realm) end

  return std.route(request)
end

-- A number that is busy, declined or does not exist is the answer, from whichever trunk gave it. Anything else
-- is the trunk failing, and the next one is tried.
local FINAL = {[404] = true, [410] = true, [484] = true, [486] = true, [600] = true, [603] = true, [604] = true}

function trunks.on_failure(request, response, state)
  if state.trunk and FINAL[response.code] then return "stop" end
  return std.on_failure(request, response, state)
end

trunks.register = std.register

-- The four hooks, for a main.lua that wants exactly this.
function trunks.hooks()
  return trunks.authorize, trunks.route, trunks.on_failure, trunks.register
end

return trunks
