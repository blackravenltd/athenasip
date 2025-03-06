--
-- AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
--
-- Copyright (C) 2025 Tom Cully <mail@tomcully.com>
-- Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
--

-- include other files
include('~/.athenasip/auth.lua')

-- procedural code is run.
print("Routing Loaded")

-- The main() function is called when AthenaSIP loads or reloads the scripts.
function main(server)
    log.info("Realm is "..server)

    my_global_server = server
end

-- Lets assume this is called by AthenaSIP when something happens.
function another_called_function()
    log.info("Realm is STILL "..my_global_server)
end

function route_call(from, to)
    if to == "sip:destination@other.com" then
        return "sip:reroute@backup.server"
    else
        return to 
    end
end
