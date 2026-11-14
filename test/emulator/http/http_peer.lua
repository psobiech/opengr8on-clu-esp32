local name, peer, nonce = "@NAME@", "@PEER@", "@NONCE@"
local listener = GATE:new(120, "HTTP_LISTENER_" .. name)
local request = GATE:new(121, "HTTP_REQUEST_" .. name)
local received = 0
local completed = 0
local sequence = 0
local target_port = 0
local mode

listener:set(0, "/echo")
listener:set(6, 1)
listener:add_event(0, function()
  local method, body = listener:get(1), listener:get(5)
  local expected = nonce .. ":" .. peer .. ":post:" .. (received + 1)
  assert(method == "POST" and body == expected, "bad HTTP request")
  received = received + 1
  print("HTTP_RECEIVED " .. name .. " " .. body)
  listener:set(6, 1)
  listener:set(8, "ACK:" .. nonce .. ":" .. name .. ":" .. received)
  listener:set(9, 200)
  listener:execute(0)
end)

request:set(4, 8)
request:set(5, 1)
request:set(6, 1)
request:add_event(1, function()
  local status, body = request:get(11), request:get(10)
  if mode == "exchange" then
    local expected = "ACK:" .. nonce .. ":" .. peer .. ":" .. sequence
    assert(status == 200 and body == expected, "bad HTTP response")
    completed = completed + 1
    print("HTTP_REPLY_OK " .. name .. " " .. sequence .. " " .. nonce)
    if sequence < 3 then
      local next_sequence = sequence + 1
      sequence = next_sequence
      request:set(1, "/echo")
      request:set(3, "POST")
      request:set(8, nonce .. ":" .. name .. ":post:" .. next_sequence)
      request:execute(0)
    else
      mode = "missing"
      request:set(1, "/not-found")
      request:set(3, "GET")
      request:set(8, nil)
      request:execute(0)
    end
  else
    assert(mode == "missing" and status == 404 and body == "Not Found", "bad HTTP 404 response")
    print("HTTP_404_OK " .. name .. " " .. nonce)
    print("HTTP_DONE " .. name .. " " .. nonce)
  end
end)

function http_start(port)
  assert(not mode, "HTTP test already started")
  target_port = assert(math.tointeger(port))
  assert(target_port > 0, "bad HTTP target port")
  request:set(0, "192.168.4.1:" .. target_port)
  mode = "exchange"
  sequence = 1
  request:set(1, "/echo")
  request:set(3, "POST")
  request:set(8, nonce .. ":" .. name .. ":post:1")
  request:execute(0)
  return "started"
end

function http_completed() return completed end
function http_received() return received end

SYSTEM.Init()
print("HTTP_READY " .. name .. " " .. nonce)
