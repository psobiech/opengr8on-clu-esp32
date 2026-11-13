-- Host test for components/clu_mysensors/mysensors.lua (USER.LUA MySensors API) with a Lua stub
-- of the C __clu table. Usage: lua5.3 test_mysensors.lua
local here = arg[0]:match("(.*/)") or "./"
local print = print
local failures = 0
local function check(name, cond)
  print((cond and "ok   " or "FAIL ") .. name)
  if not cond then failures = failures + 1 end
end

-- ---- stub of the C side ------------------------------------------------------
local inbox, sent, logs, cache = {}, {}, {}, {}
local storage = os.tmpname()
os.remove(storage)

__clu = {
  root = here .. "fixtures",
  storage = storage,
  new = function() end,
  get = function() return false end,
  set = function() return false end,
  execute = function() end,
  log = function(_, msg) logs[#logs + 1] = msg end,
  loop = function() end,
  init = function() end,
  wait = function() end,
  report = function() end,
  millis = function() return 0 end,
  remote = function() end,
  ms_recv = function()
    local m = table.remove(inbox, 1)
    if m then return table.unpack(m) end
  end,
  ms_send = function(node, child, cmd, typ, payload, ack)
    sent[#sent + 1] = string.format("%d;%d;%d;%d;%d;%s", node, child, cmd, ack and 1 or 0, typ, payload)
    if cmd == 1 then cache[node .. "/" .. child .. "/" .. typ] = payload end
    return true
  end,
  ms_get = function(node, child, typ)
    local p = cache[node .. "/" .. child .. "/" .. typ]
    if p then return p, 1234 end
  end,
  ms_nodes = function() return { [12] = { sketch = "Weather", children = {} } } end,
  ms_status = function() return { connected = true, version = "2.3.2" } end,
  ms_slots = function() return { { module = 1, kind = "din", channel = 1, node = 12, child = 1 } } end,
  ms_unmap = function(node, child) return child == nil and node == 12 and 1 or 0 end,
}

assert(loadfile(here .. "../../components/clu_lua/prelude.lua"))()
assert(loadfile(here .. "../../components/clu_lua/objects.lua"))()
assert(loadfile(here .. "../../components/clu_mysensors/mysensors.lua"))()
__clu = nil

local MS = MySensors
local function rx(...) inbox[#inbox + 1] = { ... } end
local function last() return sent[#sent] end

-- ---- constants ------------------------------------------------------------------
check("constants", MS.C.C_SET == 1 and MS.S.S_TEMP == 6 and MS.S.S_BINARY == MS.S.S_LIGHT and
  MS.V.V_STATUS == 2 and MS.V.V_LIGHT == 2 and MS.V.V_TEXT == 47 and MS.I.I_REBOOT == 13 and
  MS.I.I_HEARTBEAT_RESPONSE == 22)

-- ---- handlers ---------------------------------------------------------------------
local got = {}
local h_temp = MS.on(12, 1, MS.V.V_TEMP, function(v, m) got[#got + 1] = { "temp", v, m.node } end)
MS.on(nil, nil, MS.V.V_RGB, function(v) got[#got + 1] = { "rgb", v } end)
MS.on(nil, nil, nil, function(v, m) got[#got + 1] = { "any", v, m.type } end)
local raw = {}
local h_raw = MS.onMessage(function(m) raw[#raw + 1] = m end)

rx(12, 1, 1, 0, 0, "21.5")         -- V_TEMP set
rx(12, 2, 1, 0, 0, "3")            -- other child
rx(13, 1, 1, 0, 40, "123e45")      -- V_RGB: stays text
rx(12, 255, 0, 0, 17, "2.3.2")     -- presentation: raw only
rx(12, 1, 1, 0, 47, "hello")       -- V_TEXT
SYSTEM.Loop()

check("on(node, child, vtype): numeric value, message", got[1][1] == "temp" and got[1][2] == 21.5 and got[1][3] == 12)
check("wildcard handler sees every C_SET", #got == 6)
check("other child: only the wildcard", got[3][1] == "any" and got[3][2] == 3)
check("text types stay text", got[4][1] == "rgb" and got[4][2] == "123e45" and got[6][2] == "hello")
check("onMessage sees every message", #raw == 5 and raw[4].cmd == 0 and raw[4].type == 17 and raw[4].payload == "2.3.2")

MS.off(h_temp)
MS.off(h_raw)
got, raw = {}, {}
rx(12, 1, 1, 0, 0, "22")
SYSTEM.Loop()
check("off: handler removed", #got == 1 and got[1][1] == "any" and #raw == 0)

-- handler errors are logged, the rest still run; handlers may remove themselves
local self_h
self_h = MS.on(nil, nil, nil, function() MS.off(self_h); error("boom") end)
got = {}
rx(1, 1, 1, 0, 2, "1")
SYSTEM.Loop()
check("handler error logged", #logs == 1 and logs[1]:find("boom", 1, true) ~= nil)
check("other handlers still run", #got == 1)
rx(1, 1, 1, 0, 2, "1")
got = {}
SYSTEM.Loop()
check("handler removed itself", #logs == 1 and #got == 1)

-- per tick limit
for i = 1, 20 do rx(1, i, 1, 0, 2, "1") end
got = {}
SYSTEM.Loop()
check("at most 16 messages per loop", #got == 16)
SYSTEM.Loop()
check("rest on the next loop", #got == 20)

check("on: function required", not pcall(MS.on, 1, 1, 1, nil) and not pcall(MS.onMessage, "x"))

-- ---- sending ------------------------------------------------------------------------
MS.send(13, 7, MS.V.V_STATUS, true)
check("send true -> 1", last() == "13;7;1;0;2;1")
MS.send(13, 7, MS.V.V_STATUS, false, true)
check("send false -> 0, ack", last() == "13;7;1;1;2;0")
MS.send(13, 7, MS.V.V_PERCENTAGE, 40.0)
check("integral float -> integer text", last() == "13;7;1;0;3;40")
MS.send(13, 8, MS.V.V_TEMP, 21.25)
check("float", last() == "13;8;1;0;0;21.25")
MS.send(13, 9, MS.V.V_TEXT, "Hi")
check("string", last() == "13;9;1;0;47;Hi")
MS.send(13, 9, MS.V.V_VAR1, nil)
check("nil -> 0", last() == "13;9;1;0;24;0")
MS.send(13, 9, MS.V.V_VAR2, 1e20)
check("huge number", last() == "13;9;1;0;25;1e+20")
MS.request(13, 7, MS.V.V_STATUS)
check("request -> C_REQ", last() == "13;7;2;0;2;")
MS.internal(13, 255, MS.I.I_REBOOT)
check("internal without payload", last() == "13;255;3;0;13;")
MS.internal(0, 255, MS.I.I_INCLUSION_MODE, 1)
check("internal with payload", last() == "0;255;3;0;5;1")
MS.raw(13, 1, MS.C.C_PRESENTATION, MS.S.S_LIGHT, "x", true)
check("raw", last() == "13;1;0;1;3;x")

-- ---- reading back --------------------------------------------------------------------
local v, age = MS.get(13, 8, MS.V.V_TEMP)
check("get: number, age", v == 21.25 and age == 1234)
check("get: text type", MS.get(13, 9, MS.V.V_TEXT) == "Hi")
check("get: unknown -> nil", MS.get(99, 1, 1) == nil)
check("nodes / status from C", MS.nodes()[12].sketch == "Weather" and MS.status().version == "2.3.2")
check("slots / unmap from C", MS.slots()[1].kind == "din" and MS.unmap(12) == 1 and MS.unmap(12, 3) == 0)

os.remove(storage)
os.remove((storage:gsub("%.%w+$", "")) .. ".TMP")
print(("\n%s (%d failures)"):format(failures == 0 and "PASSED" or "FAILED", failures))
os.exit(failures == 0 and 0 or 1)
