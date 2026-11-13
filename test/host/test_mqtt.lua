-- Host test for components/clu_mqtt/mqtt.lua (HA discovery, state, commands) with a Lua stub of
-- the C __clu table. Usage: lua5.3 test_mqtt.lua
local here = arg[0]:match("(.*/)") or "./"
local print = print
local failures = 0
local function check(name, cond)
  print((cond and "ok   " or "FAIL ") .. name)
  if not cond then failures = failures + 1 end
end

-- ---- stub of the C side ------------------------------------------------------
local executed, pubs, inbox = {}, {}, {}
local connected, gen, clock = false, 0, 0
sys_fails, restarts = 3, 0
local storage = os.tmpname()
os.remove(storage)

local outbox, logs = 0, {} -- fake esp-mqtt outbox bytes; C.log messages
__clu = {
  root = here .. "fixtures",
  storage = storage,
  new = function() end,
  get = function() return false end,
  set = function() return false end,
  execute = function(o, idx, ...)
    local a = { ... }
    executed[#executed + 1] = o._name .. ":" .. idx .. (a[1] ~= nil and ("(" .. tostring(a[1]) .. ")") or "")
  end,
  log = function(_, m) logs[#logs + 1] = m end,
  loop = function() end,
  init = function() end,
  wait = function() end,
  report = function() end,
  millis = function() return clock end,
  remote = function() end,
  mqtt_cfg = function()
    return { root = "grenton/CLU100000001", status = "grenton/CLU100000001/status", ha = true,
             prefix = "homeassistant", scan_ms = 100, sn = 100000001, fw = "5.13.8-2442",
             diag_ms = 30000, api = true }
  end,
  sys_info = function()
    return { heap_free = 200 * 1024 + 5, heap_min = 150 * 1024, psram_free = 7000 * 1024, rssi = -61,
             ip = "192.168.1.100", reset_reason = "power_on", tfbus_total = 2, tfbus_online = 1,
             tfbus_fails = sys_fails }
  end,
  tfbus_module = function(sn)
    if sn == 0x13ab6c81 then return { online = true, fails = 0 } end
    if sn == 0x20c85581 then return { online = false, fails = 9 } end
  end,
  sys_restart = function() restarts = restarts + 1 end,
  mqtt_state = function() return connected, gen end,
  mqtt_publish = function(t, p, r, q)
    if not connected then return false end
    pubs[#pubs + 1] = { t = t, p = p, r = r, q = q }
    return true
  end,
  mqtt_outbox = function() return outbox end,
  mqtt_recv = function()
    local m = table.remove(inbox, 1)
    if m then return m[1], m[2] end
  end,
}

assert(loadfile(here .. "../../components/clu_lua/prelude.lua"))()
assert(loadfile(here .. "../../components/clu_lua/objects.lua"))()
assert(loadfile(here .. "../../components/clu_mqtt/mqtt.lua"))()
__clu = nil

-- USER.LUA-style override before the objects exist
MQTT.expose("DOU0002", { kind = "light", name = "Ceiling" })

assert(loadfile(here .. "fixtures/om.lua"))()
SYSTEM.Init()

local function run(n)
  for _ = 1, n or 20 do
    clock = clock + 100
    SYSTEM.Loop()
  end
end

local function find(topic)
  local hit
  for _, m in ipairs(pubs) do if m.t == topic then hit = m end end
  return hit
end

local function count(prefix)
  local n = 0
  for _, m in ipairs(pubs) do if m.t:sub(1, #prefix) == prefix then n = n + 1 end end
  return n
end

local CFG = "homeassistant/"
local NODE = "/opengr8on_CLU100000001/"

-- ---- offline: nothing goes out -------------------------------------------------
run(3)
check("nothing published while disconnected", #pubs == 0)

-- ---- connect -> discovery + states ----------------------------------------------
DOU0001._f[0] = 1
DIN0001._f[0] = 0
ROL0001._f[0] = 0
ROL0001._f[7] = 55
CLU100000001._f[0], CLU100000001._f[2] = 120, 1 -- Uptime, State (C hooks on the device)
connected, gen = true, 1
run()

local sw = find(CFG .. "switch" .. NODE .. "DOU0001/config")
check("DOU -> switch discovery", sw ~= nil and sw.r == true and sw.q == 1)
check("switch uses ~ base", sw and sw.p:find('"~":"grenton/CLU100000001/DOU0001"', 1, true) ~= nil)
check("friendly name from NAME_IO", sw and sw.p:find('"name":"LIGHT A"', 1, true) ~= nil)
check("device named from NAME_CLU", sw and sw.p:find('"name":"CLU TEST"', 1, true) ~= nil)
check("availability topic", sw and sw.p:find('"availability_topic":"grenton/CLU100000001/status"', 1, true) ~= nil)
check("unique_id", sw and sw.p:find('"unique_id":"opengr8on_CLU100000001_DOU0001"', 1, true) ~= nil)
check("override: DOU0002 -> light", find(CFG .. "light" .. NODE .. "DOU0002/config") ~= nil)
check("override replaces default", find(CFG .. "switch" .. NODE .. "DOU0002/config") == nil)
check("override name", (find(CFG .. "light" .. NODE .. "DOU0002/config") or {}).p:find('"name":"Ceiling"', 1, true) ~= nil)
check("DIN -> binary_sensor", find(CFG .. "binary_sensor" .. NODE .. "DIN0001/config") ~= nil)
local ev = find(CFG .. "event" .. NODE .. "DIN0001_event/config")
check("DIN -> event entity", ev ~= nil)
check("event types listed", ev and ev.p:find('"event_types":["press","release","short_press","long_press","hold","click"]', 1, true) ~= nil)
local cov = find(CFG .. "cover" .. NODE .. "ROL0001/config")
check("ROLLER -> cover", cov ~= nil and cov.p:find('"set_position_topic":"~/position/set"', 1, true) ~= nil)
check("no entity for POW/STO/remote CLU", find(CFG .. "switch" .. NODE .. "CLU100000002/config") == nil
  and count(CFG .. "sensor" .. NODE .. "POW") == 0 and count(CFG .. "sensor" .. NODE .. "STO") == 0
  and count("grenton/CLU100000001/CLU100000002") == 0)
check("switch state ON", (find("grenton/CLU100000001/DOU0001/state") or {}).p == "ON")
check("state retained", (find("grenton/CLU100000001/DOU0001/state") or {}).r == true)
check("binary_sensor OFF", (find("grenton/CLU100000001/DIN0001/state") or {}).p == "OFF")
check("cover stopped", (find("grenton/CLU100000001/ROL0001/state") or {}).p == "stopped")
check("cover position", (find("grenton/CLU100000001/ROL0001/position") or {}).p == "55")

-- ---- internals --------------------------------------------------------------------
local st = find(CFG .. "sensor" .. NODE .. "system_clu_state/config")
check("CLU state enum sensor", st and st.p:find('"device_class":"enum"', 1, true) and st.p:find('"options":["emergency","ok","starting"]', 1, true)
  and st.p:find('"entity_category":"diagnostic"', 1, true) and not st.p:find("state_class", 1, true))
check("CLU state value", (find("grenton/CLU100000001/system/clu/state") or {}).p == "ok")
local boot = find(CFG .. "sensor" .. NODE .. "system_clu_boot_time/config")
check("CLU boot time sensor", boot and boot.p:find('"device_class":"timestamp"', 1, true) ~= nil)
check("CLU boot time value", ((find("grenton/CLU100000001/system/clu/boot_time") or {}).p or ""):match("^%d+%-%d+%-%d+T%d+:%d+:%d+%+00:00$") ~= nil)
check("CLU time_change event entity", find(CFG .. "event" .. NODE .. "system_clu_event/config") ~= nil)
check("no CLU<sn>/CLU<sn>* entity topics", count("grenton/CLU100000001/CLU100000001") == 0)
check("leaf sensors: state_topic is ~", (find(CFG .. "sensor" .. NODE .. "system_memory_heap_free/config") or {}).p:find('"state_topic":"~"', 1, true) ~= nil
  and (find(CFG .. "sensor" .. NODE .. "system_memory_heap_free/config") or {}).p:find('"~":"grenton/CLU100000001/system/memory/heap_free"', 1, true) ~= nil)
check("no flat system_* / module topics", count("grenton/CLU100000001/system_") == 0 and count("grenton/CLU100000001/13ab6c81") == 0)
check("tfbus counters", (find("grenton/CLU100000001/system/tfbus/online") or {}).p ~= nil
  and (find("grenton/CLU100000001/system/tfbus/total") or {}).p ~= nil)
check("module connectivity", (find("grenton/CLU100000001/system/tfbus/modules/13ab6c81") or {}).p == "ON"
  and (find("grenton/CLU100000001/system/tfbus/modules/20c85581") or {}).p == "OFF"
  and (find(CFG .. "binary_sensor" .. NODE .. "system_tfbus_modules_13ab6c81/config") or {}).p:find('"device_class":"connectivity"', 1, true) ~= nil
  and (find(CFG .. "binary_sensor" .. NODE .. "system_tfbus_modules_13ab6c81/config") or {}).p:find('"name":"Module 330001537"', 1, true) ~= nil)
check("heap in KiB", (find("grenton/CLU100000001/system/memory/heap_free") or {}).p == "200")
check("rssi", (find("grenton/CLU100000001/system/network/rssi") or {}).p == "-61")
check("ip text sensor", (find("grenton/CLU100000001/system/network/ip") or {}).p == "192.168.1.100"
  and not (find(CFG .. "sensor" .. NODE .. "system_network_ip/config") or {}).p:find("state_class", 1, true))
check("reset reason", (find("grenton/CLU100000001/system/clu/reset_reason") or {}).p == "power_on")
check("lua memory", tonumber((find("grenton/CLU100000001/system/memory/lua") or {}).p) ~= nil)
local rb = find(CFG .. "button" .. NODE .. "system_clu_restart/config")
check("restart button", rb and rb.p:find('"command_topic":"~/set"', 1, true) and rb.p:find('"device_class":"restart"', 1, true)
  and rb.p:find('"entity_category":"config"', 1, true))
sys_fails = 4
run(5)
check("sampled values wait for diag interval", (find("grenton/CLU100000001/system/tfbus/missed_polls") or {}).p == "3")
clock = clock + 30000
run(2)
check("sampled values after interval", (find("grenton/CLU100000001/system/tfbus/missed_polls") or {}).p == "4")
inbox = { { "grenton/CLU100000001/system/clu/restart/set", "press" }, { "grenton/CLU100000001/system/clu/restart/set", "x" } }
run(2)
check("restart button press", restarts == 1)
__fire("CLU100000001", 13)
run(1)
check("OnTimeChange event", (find("grenton/CLU100000001/system/clu/event") or {}).p == '{"event_type":"time_change"}')

-- ---- raw object API -------------------------------------------------------------------
TIM = OBJECT:new(6, "TIM0001")
local function reply(t) return (find("grenton/CLU100000001/" .. t) or {}).p end
inbox = {
  { "grenton/CLU100000001/TIM0001/feature/0/set", "2500" },
  { "grenton/CLU100000001/TIM0001/feature/1/get", "" },
  { "grenton/CLU100000001/TIM0001/execute/0", "" },
  { "grenton/CLU100000001/NOPE/execute/0", "" },
  { "grenton/CLU100000001/CLU100000002/execute/0", '["os.exit()"]' },
  { "grenton/CLU100000001/CLU100000001/feature/22/set", 'pool.ntp.org' },
  { "grenton/CLU100000001/ROL0001/execute/10", "[33]" },
  { "grenton/CLU100000001/ROL0001/execute/11", '{"a":1}' },
}
executed = {}
run(3)
check("api set + read back", reply("TIM0001/feature/0") == '{"ok":true,"value":2500}')
check("api get", reply("TIM0001/feature/1") == '{"ok":true,"value":0}')
check("api execute (Start)", reply("TIM0001/execute/0/result") == '{"ok":true,"value":null}' and TIM:get(2) == 1)
check("api unknown object", reply("NOPE/execute/0/result") == '{"ok":false,"error":"unknown object NOPE"}')
check("api refuses remote CLU", (reply("CLU100000002/execute/0/result") or ""):find('"ok":false', 1, true) ~= nil)
check("api plain-text set", reply("CLU100000001/feature/22") == '{"ok":true,"value":"pool.ntp.org"}')
check("api execute args", executed[1] == "ROL0001:10(33)" and #executed == 1)
check("api rejects object args", (reply("ROL0001/execute/11/result") or ""):find("JSON array", 1, true) ~= nil)
check("api replies not retained", find("grenton/CLU100000001/TIM0001/feature/0").r == false)

local jd = __mqtt.jdecode
check("jdecode scalars", jd("42") == 42 and math.type(jd("42")) == "integer" and jd("-1.5e1") == -15 and jd(' "a\\u00e9\\n" ') == "a\u{e9}\n" and jd("true") == true)
local arr = jd('[1, null, "x", [2]]')
check("jdecode arrays keep n", arr.n == 4 and arr[1] == 1 and arr[2] == nil and arr[3] == "x" and arr[4][1] == 2)
check("jdecode errors", select(2, jd("[1,")) ~= nil and select(2, jd("1 2")) ~= nil and select(2, jd("os.exit()")) ~= nil)

-- ---- state only on change ---------------------------------------------------------
local n0 = #pubs
run(5)
check("unchanged values not resent", #pubs == n0)
ROL0001._f[0] = 2
run(2)
check("cover closing", pubs[#pubs].t == "grenton/CLU100000001/ROL0001/state" and pubs[#pubs].p == "closing")

-- ---- commands -------------------------------------------------------------------
executed = {}
inbox = {
  { "grenton/CLU100000001/DOU0001/set", "OFF" },
  { "grenton/CLU100000001/DOU0002/set", "on" },
  { "grenton/CLU100000001/ROL0001/set", "OPEN" },
  { "grenton/CLU100000001/ROL0001/set", "STOP" },
  { "grenton/CLU100000001/ROL0001/position/set", "42.4" },
  { "grenton/CLU100000001/ROL0001/position/set", "250" },
  { "grenton/CLU100000001/DIN0001/set", "ON" },          -- no command on inputs
  { "grenton/CLU100000001/NOPE/set", "ON" },
  { "grenton/CLU100000001/DOU0001/set", "os.exit()" },   -- never evaluated
  { "grenton/OTHER/DOU0001/set", "ON" },
}
run(3)
check("commands -> object calls", table.concat(executed, " ") == "DOU0001:2 DOU0002:1 ROL0001:5 ROL0001:3 ROL0001:10(42) ROL0001:10(100)")

-- ---- events -----------------------------------------------------------------------
__fire("DIN0001", 6)
run(1)
local e = find("grenton/CLU100000001/DIN0001/event")
check("button click event", e ~= nil and e.p == '{"event_type":"click"}' and e.r == false)
__fire("DIN0001", 4); __fire("DIN0001", 4)
run(1)
local n_ev = 0
for _, m in ipairs(pubs) do if m.t == "grenton/CLU100000001/DIN0001/event" then n_ev = n_ev + 1 end end
check("events are not coalesced", n_ev == 3)

-- publisher stalled (outbox full): pending events are capped, the newest dropped and counted
outbox = 1 << 20
pubs, logs = {}, {}
for _ = 1, 200 do __fire("DIN0001", 4) end
run(1)
check("stalled: nothing sent", #pubs == 0)
check("stalled: overflow logged once", #logs == 1 and logs[1]:find("queue full", 1, true) ~= nil)
outbox = 0
run(10)
n_ev = 0
for _, m in ipairs(pubs) do if m.t == "grenton/CLU100000001/DIN0001/event" then n_ev = n_ev + 1 end end
check("stalled: queue capped at 64 events", n_ev == 64)
__fire("DIN0001", 4)
run(1)
check("drop count logged when events flow again", logs[#logs] == "mqtt: 136 event(s) dropped" and pubs[#pubs].t == "grenton/CLU100000001/DIN0001/event")

-- ---- HA restart -------------------------------------------------------------------
local before = count(CFG)
inbox = { { "homeassistant/status", "online" } }
run(3)
check("HA online -> discovery resent", count(CFG) > before)

-- ---- remap at runtime: old configs cleared -----------------------------------------
MQTT.hide(DOU0001)
run(3)
local cleared = find(CFG .. "switch" .. NODE .. "DOU0001/config")
check("hidden entity config cleared", cleared and cleared.p == "" and cleared.r == true)
__persist.flush(true)
local f = io.open(storage, "rb")
local saved = f and f:read("a") or ""
if f then f:close() end
check("published configs persisted", saved:find("DOU0002/config", 1, true) ~= nil and saved:find("switch/opengr8on_CLU100000001/DOU0001", 1, true) == nil)

-- ---- reconnect -> full republish ----------------------------------------------------
connected = false
run(2)
pubs = {}
connected, gen = true, 2
run()
check("reconnect republishes states", find("grenton/CLU100000001/ROL0001/position") ~= nil)
check("reconnect republishes discovery", find(CFG .. "cover" .. NODE .. "ROL0001/config") ~= nil)

-- ---- script API / JSON ---------------------------------------------------------------
check("MQTT.connected", MQTT.connected() == true)
check("MQTT.publish", MQTT.publish("x/y", 5, true) and pubs[#pubs].t == "x/y" and pubs[#pubs].p == "5")
check("json escaping", __mqtt.json({ a = 'q"\\\n\1' }) == '{"a":"q\\"\\\\\\n\\u0001"}')
check("expose rejects unknown kind", not pcall(MQTT.expose, DOU0001, "toaster"))

os.remove(storage)
os.remove((storage:gsub("%.%w+$", "")) .. ".TMP")
print(failures == 0 and "all ok" or (failures .. " failure(s)"))
os.exit(failures == 0 and 0 or 1)
