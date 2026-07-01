--
-- AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
--
-- SIP Authentication Flow for REGISTER Requests
--
-- This script assumes that:
--   • The incoming SIP message is provided as a Lua table (msg)
--   • msg.header is a table with helper functions "get" and "set"
--   • A global nonce object is available with functions:
--         nonce.generate()  -- returns a new nonce string
--         nonce.check(auth) -- returns true if the auth header is valid
--   • A function send_message(response) is available to send SIP responses
--

function route_message(msg)
    if not msg then
        log.error("No SIP message provided")
        return
    end

    local method = (msg.header and msg.header.request_method) or "UNKNOWN"
    log.info("Processing SIP message with Call-ID: " .. (msg.callID or "unknown") .. " (method: " .. method .. ")")

    if method == "REGISTER" then
        -- For REGISTER, enforce authentication.
        local auth = msg.header.get("Authorization")
        if not auth or auth == "" then
            log.warn("No Authorization header found in REGISTER")
            local new_nonce = nonce.generate()
            send_unauthorized(msg, new_nonce)
            return
        end

        if not nonce.check(auth) then
            log.warn("Authorization header check failed")
            local new_nonce = nonce.generate()
            send_unauthorized(msg, new_nonce)
            return
        end

        log.info("Authentication successful for REGISTER")
        -- Continue with registration processing.
        register_endpoint(msg)

    elseif method == "INVITE" then
        -- Handle other SIP methods as needed.
        initiate_call(msg)
    else
        log.info("No special processing for method: " .. method)
    end
end

-- Sends a 401 Unauthorized response with a WWW-Authenticate header.
function send_unauthorized(msg, nonce_value)
    log.info("Sending 401 Unauthorized response with nonce: " .. nonce_value)
    -- Generate a SIP response from the incoming message.
    local response = msg.generate_response()
    response.response_code = 401
    response.response_message = "Unauthorized"
    
    -- Build the WWW-Authenticate header.
    local realm = "example.com"  -- Replace with your realm.
    local www_authenticate = 'Digest realm="' .. realm .. '", nonce="' .. nonce_value .. '"'
    response.header.set("WWW-Authenticate", www_authenticate)
    
    -- Publish the response.
    send_message(response)
end

-- Stub: Registers the endpoint. Actual implementation should be added.
function register_endpoint(msg)
    log.info("Registering endpoint for " .. (msg.header.get("From") or "unknown"))
    -- Actual registration logic goes here.
end

-- Stub: Initiates a call for INVITE requests. Actual implementation should be added.
function initiate_call(msg)
    log.info("Initiating call from " .. (msg.header.get("From") or "unknown") ..
             " to " .. (msg.header.get("To") or "unknown"))
    -- Actual call setup logic goes here.
end
