--
-- AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
--
-- Copyright (C) 2026 Tom Cully <mail@tomcully.com>
-- Licensed under the GNU GPLv3 - see <https://www.gnu.org/licenses/gpl-3.0.html>
--
-- Run by the node before any script, to make the answers a hook returns. Each is a plain table the node reads
-- back; building one by hand works too, but these check what they are given and say where a mistake is.

local a = athenasip

local function check(condition, message)
  if not condition then error(message, 3) end
end

local function is_realm(value) return type(value) == "userdata" and a.is_realm(value) end

a.auth = {}

-- Nothing to prove: an in-dialog request, a peer, or a caller the script has decided to let through.
function a.auth.accept() return {decision = "auth", kind = "accept"} end

-- RFC 3261 22.3: the node challenges, and verifies the answer, in this realm. realm = "*" is any realm here, with
-- one challenge for each. from_must_match (default true) requires the credentials to be the From's subscriber's.
function a.auth.digest(options)
  check(type(options) == "table", "auth.digest takes a table: {realm = realm}")
  check(options.realm == "*" or is_realm(options.realm), "auth.digest: realm is a realm from athenasip.store, or \"*\"")
  return {
    decision = "auth",
    kind = "digest",
    realm = options.realm ~= "*" and options.realm or nil,
    from_must_match = options.from_must_match ~= false,
  }
end

-- A request from a trunk, proved by where it came from (trunk:admits). route then sees request.trunk.
function a.auth.trusted(options)
  check(type(options) == "table" and type(options.trunk) == "string", "auth.trusted takes {trunk = name}")
  return {decision = "auth", kind = "trusted", trunk = options.trunk}
end

function a.auth.reject(code, reason)
  check(math.type(code) == "integer" and code >= 300 and code <= 699, "auth.reject: the code is a final failure, 300 to 699")
  return {decision = "auth", kind = "reject", code = code, reason = reason}
end

a.route = {}

-- Every device of an address of record, in the node's order, pushed or reached through a peer node as needed.
-- options.bindings: the bindings already read from athenasip.store, so the node need not read them again.
-- options.ring_timeout, on this and the others: seconds an INVITE may ring there before the node moves on.
function a.route.subscriber(subscriber, options)
  check(type(subscriber) == "userdata" and a.is_subscriber(subscriber), "route.subscriber: a subscriber from athenasip.store")
  options = options or {}
  return {target = "subscriber", subscriber = subscriber, bindings = options.bindings, ring_timeout = options.ring_timeout}
end

-- One URI, located by RFC 3263. options.next_hop: where to send it, when not where the URI says. options.trunk:
-- the trunk it leaves by, whose challenges the node answers.
function a.route.uri(uri, options)
  check(uri ~= nil, "route.uri: a URI, or its text")
  options = options or {}
  return {target = "uri", uri = uri, next_hop = options.next_hop, trunk = options.trunk, ring_timeout = options.ring_timeout}
end

-- Out by a trunk: the trunk's URI with options.user (the number dialled) as its user part, sent to its outbound
-- proxy when it has one.
function a.route.trunk(trunk, options)
  check(type(trunk) == "userdata" and a.is_trunk(trunk), "route.trunk: a trunk from athenasip.store")
  options = options or {}
  local uri = trunk.uri
  if options.user then uri = uri:with{user = options.user} end
  return {target = "uri", uri = uri, next_hop = trunk.proxy, trunk = trunk.name, ring_timeout = options.ring_timeout}
end

-- The targets, tried in turn. options.media: {media_anchor =, media_profile =}, as athenasip.config.behaviour and
-- a realm's behaviour_effective give it. options.rewrite_contact: whether Contacts are rewritten for this call.
function a.route.forward(targets, options)
  check(type(targets) == "table", "route.forward: a list of targets")
  options = options or {}
  return {decision = "route", kind = "forward", targets = targets, media = options.media, rewrite_contact = options.rewrite_contact}
end

-- A final response in the node's place.
function a.route.reply(code, reason)
  check(math.type(code) == "integer" and code >= 200 and code <= 699, "route.reply: the code is a final response, 200 to 699")
  return {decision = "route", kind = "reply", code = code, reason = reason}
end

a.route.reject = a.route.reply

a.register = {}

-- Register into this realm. max_expires is the longest lifetime granted, min_expires the shortest accepted (0 for
-- none), qualify the seconds between OPTIONS to each device (0 for none).
function a.register.accept(options)
  check(type(options) == "table" and is_realm(options.realm), "register.accept: {realm = a realm from athenasip.store}")
  return {
    decision = "register",
    kind = "accept",
    realm = options.realm,
    max_expires = options.max_expires or 3600,
    min_expires = options.min_expires or 0,
    qualify = options.qualify or 0,
  }
end

-- RFC 3261 10.3 step 1: forward it to the domain it names.
function a.register.forward() return {decision = "register", kind = "forward"} end

function a.register.reject(code, reason)
  check(math.type(code) == "integer" and code >= 300 and code <= 699, "register.reject: the code is a final failure, 300 to 699")
  return {decision = "register", kind = "reject", code = code, reason = reason}
end
