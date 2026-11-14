local name, peer, nonce = "@NAME@", "@PEER@", "@NONCE@"
local remote = OBJECT:new(1, "192.168.4.1", "REMOTE")
udp_received, udp_completed = 0, 0

function udp_ping(token, seq)
  assert(token == nonce .. ":" .. peer .. ":ping:" .. seq, "bad UDP request")
  assert(seq == udp_received + 1, "bad UDP sequence")
  udp_received = seq
  print("UDP_RECEIVED " .. name .. " " .. token)
  return nonce .. ":" .. name .. ":pong:" .. seq
end

local started = {}

-- Idempotent: the runner retries a start whose UDP reply was lost; only the first call spawns.
function udp_start(drop)
  local mode = drop and "drop" or "exchange"
  if started[mode] then return "started" end
  started[mode] = true
  __spawn("UDP test", function()
    local count = drop and 1 or 3
    for i = 1, count do
      local token = nonce .. ":" .. name .. ":ping:" .. i
      local script = "return udp_ping(" .. string.format("%q", token) .. "," .. i .. ")"
      local result = remote:execute(0, script)
      if drop then
        assert(result == "error", "missing timeout error")
        print("UDP_TIMEOUT_OK " .. name .. " " .. nonce)
      else
        assert(result == nonce .. ":" .. peer .. ":pong:" .. i, "bad UDP reply: " .. tostring(result))
        udp_completed = i
        print("UDP_REPLY_OK " .. name .. " " .. i .. " " .. nonce)
      end
    end
    if not drop then print("UDP_DONE " .. name .. " " .. nonce) end
  end)
  return "started"
end

SYSTEM.Init()
print("UDP_READY " .. name .. " " .. nonce)
