--
-- AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
--
-- Copyright (C) 2026 Tom Cully <mail@tomcully.com>
-- Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
--
-- The standard policy: exactly what builtin:// does (src/policy/builtin_policy.cpp), written as a script. A node
-- serving its own realms and nothing more. tests/policy keeps the two in step; change one and the other must
-- follow.

local a = athenasip
local std = {}

-- This node is not an open relay. A caller claiming one of this node's realms must authenticate; any other
-- caller may only reach this node's realms.
function std.authorize(request)
  -- RFC 3261 22.1: ACK and CANCEL cannot be challenged.
  if request.method == "ACK" or request.method == "CANCEL" then return a.auth.accept() end

  -- A peer node has already authorised the caller. Trust comes from the certificate, never from a Via or a Route.
  if request.from_peer then return a.auth.accept() end

  -- In-dialog requests of an authorised call pass. The dialog table decides; a To tag alone proves nothing.
  if request.in_dialog then return a.auth.accept() end

  -- Routed back through a Path or Record-Route this node wrote: nobody else could have sealed the token.
  if request.has_flow_token then return a.auth.accept() end

  -- RFC 3261 10.3 step 1: the From is the foreign address of record, so a subscriber of any realm here may relay.
  if request.relay then return a.auth.digest{realm = "*", from_must_match = false} end

  local from = request.from
  if not from then
    a.log.info("Request with no usable From - 400")
    return a.auth.reject(400, "Bad Request")
  end

  local realm = a.store.realm(from.uri.host:lower())
  if realm then return a.auth.digest{realm = realm} end

  -- An unknown caller may not route a request off this node.
  if request.has_route then
    a.log.info("Request from " .. tostring(from.uri) .. " routed off this node - 403")
    return a.auth.reject(403, "Forbidden")
  end

  -- Anybody may call into this node's realms.
  if a.store.realm(request.uri.host:lower()) then return a.auth.accept() end

  a.log.info("Request from " .. tostring(from.uri) .. " to " .. tostring(request.uri) .. ", neither of them here - 403")
  return a.auth.reject(403, "Forbidden")
end

-- RFC 3261 16.5: a Request-URI in a realm here is an address of record, whose bindings are the targets. Any other
-- is itself the only target.
function std.route(request)
  local called = request.uri
  local realm = a.store.realm(called.host:lower())

  if not realm then
    local server = a.config.behaviour
    return a.route.forward({a.route.uri(called)}, {media = server, rewrite_contact = server.rewrite_contact})
  end

  local subscriber = a.store.subscriber(called)
  if not subscriber then
    a.log.info("No subscriber for " .. tostring(called) .. " - 404")
    return a.route.reply(404, "Not Found")
  end

  local bindings = a.store.locations(subscriber)
  if #bindings == 0 then
    a.log.info("No bindings for " .. tostring(called) .. " - 480")
    return a.route.reply(480, "Temporarily Unavailable")
  end

  local behaviour = realm.behaviour_effective
  return a.route.forward({a.route.subscriber(subscriber, {bindings = bindings})}, {media = behaviour, rewrite_contact = behaviour.rewrite_contact})
end

-- Every 3xx to 5xx moves on to the next target. 2xx and 6xx end the fork before this is asked.
function std.on_failure(request, response, state)
  return "next"
end

-- RFC 3261 10.3: the realm is the To's domain. A REGISTER for a domain not served here is forwarded there when
-- sip.forward_register allows (step 1); one whose Request-URI names this node or a realm here is step 5's 404.
function std.register(request)
  local to = request.to
  if not to then return a.register.reject(400, "Bad Request") end

  local realm = a.store.realm(to.uri.host:lower())
  if realm then
    return a.register.accept{
      realm = realm,
      max_expires = realm.registration_timeout,
      min_expires = realm.registration_minimum,
      qualify = realm.behaviour_effective.qualify_interval,
    }
  end

  local function unserved()
    a.log.info("REGISTER for unserved domain " .. to.uri.host .. " - 404")
    return a.register.reject(404, "Not Found")
  end

  local uri = request.uri
  if not uri then return unserved() end

  local port = uri.port or (uri.scheme:lower() == "sips" and 5061 or 5060)
  if a.node.names(uri.host, port) then return unserved() end
  if a.store.realm(uri.host:lower()) then return unserved() end

  if a.config.get("sip.forward_register") ~= "subscribers" then
    a.log.info("REGISTER for " .. tostring(uri) .. ", a domain not served here, and sip.forward_register is never - 403")
    return a.register.reject(403, "Forbidden")
  end

  return a.register.forward()
end

return std
