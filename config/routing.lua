function route_call(from, to)
    if to == "sip:destination@other.com" then
        return "sip:reroute@backup.server"
    else
        return to  -- Default behavior: forward as-is
    end
end

function main(server)
    print(server)
end