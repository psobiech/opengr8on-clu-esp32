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
-- Bus master view of the TF-Bus modules (versions once VERSION_QUERY answered)
local module_statuses = {
  [0x13ab6c81] = { online = true, fails = 0, hw_ver = 2, fw_ver = "2.2.4" },
  [0x20c85581] = { online = false, fails = 9 },
  [0x30000001] = { online = true, fails = 0 }, -- SMART PANEL
}
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
             prefix = "homeassistant", scan_ms = 100, sn = 100000001, fw = "v1.4.0-3-gabcdef",
             model = "Test board", hw = "esp32s3 v0.2", diag_ms = 30000, api = true }
  end,
  sys_info = function()
    return { heap_free = 200 * 1024 + 5, heap_min = 150 * 1024, psram_free = 7000 * 1024, rssi = -61,
             ip = "192.168.1.100", reset_reason = "task_watchdog", tfbus_total = 2, tfbus_online = 1,
             last_crash = "task watchdog: clu_lua (CPU 1) | task IDLE1 | pc 0x400d1234",
             tfbus_fails = sys_fails, tfbus_load = 3.4, tfbus_busy = 62.5,
             cpu_load_0 = 12.5, cpu_load_1 = 40.5 }
  end,
  tfbus_module = function(sn) return module_statuses[sn] end,
  ms_slots = function() return { { module = 1, sn = 0x0a000004, kind = "din", channel = 1 } } end, -- MySensors
  -- On-board modules: GPIO input module (DIN1 + DIN3 wired, DOUT drive nothing), ANALOG_DIN
  -- (AnalogIN 1 set up, 1-Wire hub); the ONE_WIRE sensor module and the rest are not restricted.
  local_channels = function(sn)
    if sn == 0x0a000001 then return { [3] = 0x05, [4] = 0, [12] = 0, [73] = 0 } end
    if sn == 0x0a000002 then return { [3] = 0, [4] = 0, [12] = 0x01, [73] = 1 } end
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
local mqtt_dir = here .. "../../components/clu_mqtt/"
local parts_file = assert(io.open(mqtt_dir .. "lua/parts.txt", "rb"))
local mqtt_source = {}
for part in parts_file:lines() do
  if part ~= "" then
    local part_file = assert(io.open(mqtt_dir .. "lua/" .. part, "rb"))
    mqtt_source[#mqtt_source + 1] = part_file:read("*a")
    part_file:close()
  end
end
parts_file:close()
assert(load(table.concat(mqtt_source), "@" .. mqtt_dir .. "mqtt.lua"))()
__clu = nil

-- USER.LUA-style override before the objects exist
MQTT.expose("DOU0002", { kind = "light", name = "Ceiling" })
MQTT.expose("LDO0002", "switch") -- on-board output without hardware, exposed on purpose

assert(loadfile(here .. "fixtures/om.lua"))()
DIM0001 = OBJECT:new(5, mm_550000001, 0, "DIM0001")
LED0001 = OBJECT:new(9, mm_550000001, 0, "LED0001")
LCH0001 = OBJECT:new(49, mm_550000001, 3, "LCH0001")
CCT0001 = OBJECT:new(72, mm_550000001, 0, "CCT0001")
TMR0001 = OBJECT:new(6, "TMR0001")
SCH0001 = OBJECT:new(16, "SCH0001")
THE0001 = OBJECT:new(18, "THE0001")
PRE0001 = OBJECT:new(40, "PRE0001")
mm_gpio = OBJECT:new(2, 0x0a000001, 0x1e, "0a000001")
LDI0001 = OBJECT:new(3, mm_gpio, 0, "LDI0001")   -- GPIO input
LDI0002 = OBJECT:new(3, mm_gpio, 1, "LDI0002")   -- 1-Wire bus pin
LDO0001 = OBJECT:new(4, mm_gpio, 0, "LDO0001")
LDO0002 = OBJECT:new(4, mm_gpio, 1, "LDO0002")
LPO0001 = OBJECT:new(12, mm_gpio, 0, "LPO0001")  -- PowerSupplyVoltage
mm_analog = OBJECT:new(2, 0x0a000002, 0x19, "0a000002")
AIN0001 = OBJECT:new(12, mm_analog, 0, "AIN0001")
AIN0002 = OBJECT:new(12, mm_analog, 1, "AIN0002") -- not set up
AOU0001 = OBJECT:new(13, mm_analog, 0, "AOU0001")
mm_onewire = OBJECT:new(2, 0x0a000003, 0xff, "0a000003")
ONS0001 = OBJECT:new(23, mm_onewire, 0, "ONS0001")
HUB0001 = OBJECT:new(73, mm_analog, 0, "HUB0001")
mm_panel = OBJECT:new(2, 0x30000001, 0x03, "30000001")
PAN0001 = OBJECT:new(29, mm_panel, 0, "PAN0001")
PBT0001 = OBJECT:new(30, mm_panel, 0, "PBT0001")
CAL0001 = OBJECT:new(15, "CAL0001")
SUN0001 = OBJECT:new(39, "SUN0001")
STA0001 = OBJECT:new(47, "STA0001")
PUL0001 = OBJECT:new(48, "PUL0001")
mm_mysensors = OBJECT:new(2, 0x0a000004, 0x1e, "0a000004") -- not on the bus yet
MSI0001 = OBJECT:new(3, mm_mysensors, 0, "MSI0001")
-- one more CLU user feature, as OM declares it (the fixture has number and string ones)
_type["flag"] = "boolean"
flag = true
TMR0001:set(0, 60000)
TMR0001:execute(0)
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
local DEVICE_CFG = CFG .. "device/"
local NODE = "opengr8on_CLU100000001"

-- Latest discovery of a component: { p = its config as JSON (sorted keys, as published:
-- abbreviated), node = the decoded device config holding it, t / r / q of that message }.
-- A component removed from its device shows as p = '{"p":"<platform>"}'.
local function config(platform, id)
  for message_index = #pubs, 1, -1 do
    local message = pubs[message_index]
    if message.t:sub(1, #DEVICE_CFG) == DEVICE_CFG and message.p ~= "" then
      local node = __mqtt.jdecode(message.p)
      local component = node and node.cmps[platform .. "_" .. id]
      if component then
        return { p = __mqtt.json(component), node = node, t = message.t, r = message.r, q = message.q }
      end
    end
  end
end
local function encoded(value) return value and __mqtt.json(value) or "" end

-- ---- offline: nothing goes out -------------------------------------------------
run(3)
check("nothing published while disconnected", #pubs == 0)

-- ---- connect -> discovery + states ----------------------------------------------
DOU0001._f[0] = 1
DIN0001._f[0] = 0
ROL0001._f[0] = 0
ROL0001._f[7] = 55
DIM0001._f[0] = 0.5
LED0001._f[3], LED0001._f[4], LED0001._f[5], LED0001._f[15] = 200, 100, 0, 50
LCH0001._f[0] = 77
CCT0001._f[0], CCT0001._f[2] = 4600, 0
POW0001._f[0] = 23.8
SCH0001._f[1], SCH0001._f[2] = 1, 42
THE0001._f[3], THE0001._f[14] = 21.5, 20.25
AIN0001._f[0] = 3.3
ONS0001._f[0] = 21.5
PBT0001._f[1] = 0 -- Mode Monostable
CLU100000001._f[0], CLU100000001._f[2] = 120, 1 -- Uptime, State (C hooks on the device)
-- per-entity configs of an earlier firmware, persisted; a panel button published in its panel's config
__persist.section("mqtt").configs = { "homeassistant/switch/opengr8on_CLU100000001/DOU0001/config" }
local PANEL_CFG = DEVICE_CFG .. NODE .. "_30000001/config"
__persist.section("mqtt").devices = { [PANEL_CFG] = { event_PBT0001_event = "event" } }
connected, gen = true, 1
run()
local panel_config_index, button_config_index
for message_index, message in ipairs(pubs) do
  if message.t == PANEL_CFG and not panel_config_index then panel_config_index = message_index end
  if message.t == DEVICE_CFG .. NODE .. "_PBT0001/config" and not button_config_index then button_config_index = message_index end
end
check("entity moved to its own device: removed from the old config first", panel_config_index and button_config_index
  and panel_config_index < button_config_index
  and pubs[panel_config_index].p:find('"event_PBT0001_event":{"p":"event"}', 1, true) ~= nil)
check("per-entity configs of earlier firmware cleared once",
  (find("homeassistant/switch/opengr8on_CLU100000001/DOU0001/config") or {}).p == ""
  and __persist.section("mqtt").configs == nil)
local largest_config = 0
for _, message in ipairs(pubs) do
  if message.t:sub(1, #DEVICE_CFG) == DEVICE_CFG then largest_config = math.max(largest_config, #message.p) end
end
print(string.format("info largest device config: %d bytes, %d device configs", largest_config, count(DEVICE_CFG)))
local LONG_KEYS = { unique_id = true, device_class = true, unit_of_measurement = true, entity_category = true,
                    state_class = true, availability = true, availability_topic = true, event_types = true,
                    supported_color_modes = true, options = true, icon = true }
local long_keys = {}
for _, message in ipairs(pubs) do
  if message.t:sub(1, #DEVICE_CFG) == DEVICE_CFG and message.p ~= "" then
    for component_key, component in pairs(__mqtt.jdecode(message.p).cmps) do
      for key in pairs(component) do
        if LONG_KEYS[key] or key:match("_topic$") or key:match("_template$") then
          long_keys[#long_keys + 1] = component_key .. "." .. key
        end
      end
    end
  end
end
check("every component key abbreviated (" .. table.concat(long_keys, ", ") .. ")", #long_keys == 0)

local sw = config("switch", "DOU0001")
check("DOU -> switch discovery", sw ~= nil and sw.r == true and sw.q == 1)
check("switch uses ~ base", sw and sw.p:find('"~":"grenton/CLU100000001/DOU0001"', 1, true) ~= nil)
check("friendly name from NAME_IO", sw and sw.p:find('"name":"LIGHT A"', 1, true) ~= nil)
check("module IO in its module's device config (model, serial, TF-Bus versions)", sw
  and sw.t == DEVICE_CFG .. NODE .. "_13ab6c81/config" and encoded(sw.node.dev)
  == '{"hw":"2","ids":["opengr8on_CLU100000001_13ab6c81"],"mdl":"IO MODULE DIN 8","mf":"Grenton","name":"IO MODULE DIN 8 330001537","sn":"330001537","sw":"2.2.4","via_device":"opengr8on_CLU100000001"}')
check("origin with the build version", sw and encoded(sw.node.o) == '{"name":"opengr8on-clu-esp32","sw":"v1.4.0-3-gabcdef"}')
local tmr_cfg = config("sensor", "TMR0001")
check("virtual object in the CLU's config, named from NAME_CLU", tmr_cfg
  and tmr_cfg.t == DEVICE_CFG .. NODE .. "/config" and tmr_cfg.node.dev.name == "CLU TEST")
local rol_cfg = config("cover", "ROL0001")
check("other module: own device config", rol_cfg and rol_cfg.t == DEVICE_CFG .. NODE .. "_20c85581/config"
  and rol_cfg.node.dev.name == "ROLLER SH DIN 3 550000001")
check("TF-Bus module: available while the CLU is online and the module's link is ON (device level)", sw
  and encoded(sw.node.avty) == '[{"t":"grenton/CLU100000001/status"},{"pl_avail":"ON","pl_not_avail":"OFF","t":"grenton/CLU100000001/system/tfbus/modules/13ab6c81"}]'
  and sw.node.avty_mode == "all" and sw.node.avty_t == nil and not sw.p:find('"avty', 1, true))
check("CLU config: CLU availability only", tmr_cfg and tmr_cfg.node.avty_t == "grenton/CLU100000001/status"
  and tmr_cfg.node.avty == nil)
check("CLU device: board model, chip, build version", tmr_cfg and encoded(tmr_cfg.node.dev)
  == '{"hw":"esp32s3 v0.2","ids":["opengr8on_CLU100000001"],"mdl":"Test board","mf":"OpenGr8on","name":"CLU TEST","sn":"100000001","sw":"v1.4.0-3-gabcdef"}')
check("component: platform, abbreviated keys", sw and sw.p == '{"cmd_t":"~/set","name":"LIGHT A","p":"switch","stat_t":"~/state","uniq_id":"opengr8on_CLU100000001_DOU0001","~":"grenton/CLU100000001/DOU0001"}')
check("unique_id", sw and sw.p:find('"uniq_id":"opengr8on_CLU100000001_DOU0001"', 1, true) ~= nil)
check("override: DOU0002 -> light", config("light", "DOU0002") ~= nil)
check("override replaces default", config("switch", "DOU0002") == nil)
check("override name", (config("light", "DOU0002") or {}).p:find('"name":"Ceiling"', 1, true) ~= nil)
check("DIN -> binary_sensor", config("binary_sensor", "DIN0001") ~= nil)
local ev = config("event", "DIN0001_event")
check("DIN -> event entity", ev ~= nil)
check("event types listed", ev and ev.p:find('"evt_typ":["press","release","short_press","long_press","hold","click"]', 1, true) ~= nil)
local cov = config("cover", "ROL0001")
check("ROLLER -> cover", cov ~= nil and cov.p:find('"set_pos_t":"~/position/set"', 1, true) ~= nil)
local dim = config("light", "DIM0001")
check("DIMM -> brightness light", dim and dim.p:find('"schema":"json"', 1, true)
  and dim.p:find('"sup_clrm":["brightness"]', 1, true) and dim.p:find('"cmd_t":"~/set"', 1, true))
check("DIMM state", (find("grenton/CLU100000001/DIM0001/state") or {}).p == '{"brightness":128,"color_mode":"brightness","state":"ON"}')
local led = config("light", "LED0001")
check("LEDRGB -> rgbw light", led and led.p:find('"sup_clrm":["rgbw"]', 1, true))
local lch = config("light", "LCH0001")
check("LED_CHANNEL -> brightness light", lch and lch.p:find('"sup_clrm":["brightness"]', 1, true))
check("LED_CHANNEL state", (find("grenton/CLU100000001/LCH0001/state") or {}).p == '{"brightness":77,"color_mode":"brightness","state":"ON"}')
local cct = config("light", "CCT0001")
check("LED_CCT -> color_temp light", cct and cct.p:find('"sup_clrm":["color_temp"]', 1, true)
  and cct.p:find('"clr_temp_k":true', 1, true) and cct.p:find('"min_k":2700', 1, true) and cct.p:find('"max_k":6500', 1, true))
check("LED_CCT state (off)", (find("grenton/CLU100000001/CCT0001/state") or {}).p == '{"color_mode":"color_temp","color_temp":4600,"state":"OFF"}')
check("LEDRGB state", (find("grenton/CLU100000001/LED0001/state") or {}).p
  == '{"brightness":200,"color":{"b":0,"g":128,"r":255,"w":64},"color_mode":"rgbw","state":"ON"}')
local led_event, led_ramp = config("event", "LED0001_event"), config("number", "LED0001_3")
check("LEDRGB -> events, Ramp Time and R/G/B Correction numbers (config) on the module device",
  led_event and led_event.p:find('"evt_typ":["value_change","switch_on","switch_off","value_rise","value_lower","out_of_range"]', 1, true)
  and not led_event.p:find('"dev_cla"', 1, true)
  and led_event.t == led.t
  and led_ramp and led_ramp.p:find('"name":"LED0001 Ramp Time"', 1, true) and led_ramp.p:find('"max":15000', 1, true)
  and led_ramp.p:find('"ent_cat":"config"', 1, true)
  and (config("number", "LED0001_4") or {}).p:find('"name":"LED0001 Red Correction"', 1, true)
  and (config("number", "LED0001_5") or {}).p:find('"name":"LED0001 Green Correction"', 1, true)
  and (config("number", "LED0001_6") or {}).p:find('"name":"LED0001 Blue Correction"', 1, true)
  and (config("number", "LED0001_6") or {}).p:find('"max":100', 1, true))
check("LED_CHANNEL: own device under the module (light, events, Ramp Time)",
  lch and lch.t == DEVICE_CFG .. NODE .. "_LCH0001/config" and not lch.p:find('"name"', 1, true)
  and encoded(lch.node.dev.via_device) == '"opengr8on_CLU100000001_20c85581"'
  and (config("event", "LCH0001_event") or {}).t == lch.t
  and (config("number", "LCH0001_3") or {}).p:find('"name":"Ramp Time"', 1, true))
check("LED_CCT: own device, colour temperature event",
  cct and cct.t == DEVICE_CFG .. NODE .. "_CCT0001/config"
  and (config("event", "CCT0001_event") or {}).p:find('"evt_typ":["value_change","color_temp_change","switch_on","switch_off","out_of_range"]', 1, true))
check("no entity for POW/STO/remote CLU", config("switch", "CLU100000002") == nil
  and config("sensor", "STO99999") == nil
  and count("grenton/CLU100000001/CLU100000002") == 0)
local voltage = config("sensor", "POW0001")
check("PowerSupplyVoltage -> voltage sensor", voltage and voltage.p:find('"dev_cla":"voltage"', 1, true)
  and voltage.p:find('"unit_of_meas":"V"', 1, true)
  and (find("grenton/CLU100000001/POW0001/state") or {}).p == "23.8")
check("on-board input with hardware -> binary_sensor + event",
  config("binary_sensor", "LDI0001") ~= nil and config("event", "LDI0001_event") ~= nil)
check("on-board channels without hardware -> no entities",
  config("binary_sensor", "LDI0002") == nil and config("event", "LDI0002_event") == nil
  and config("switch", "LDO0001") == nil and config("sensor", "LPO0001") == nil
  and config("sensor", "AIN0002") == nil
  and count("grenton/CLU100000001/LDO0001") == 0 and count("grenton/CLU100000001/LPO0001") == 0)
check("USER.LUA override exposes an on-board channel without hardware", config("switch", "LDO0002") ~= nil)
local analog_in = config("sensor", "AIN0001")
check("ANALOG_DIN AnalogIN -> sensor without voltage class / unit", analog_in
  and not analog_in.p:find('"dev_cla"', 1, true) and not analog_in.p:find('"unit_of_meas"', 1, true)
  and analog_in.node.dev.name == "ANALOG DIN 167772162"
  and (find("grenton/CLU100000001/AIN0001/state") or {}).p == "3.3")
local on_board_input = config("binary_sensor", "LDI0001")
check("on-board module: emulated (OpenGr8on), no link availability", on_board_input
  and on_board_input.node.dev.mf == "OpenGr8on" and on_board_input.node.avty == nil
  and on_board_input.node.avty_t == "grenton/CLU100000001/status")
local onewire = config("sensor", "ONS0001")
check("ONEW_SENSOR -> temperature sensor", onewire and onewire.p:find('"dev_cla":"temperature"', 1, true)
  and onewire.p:find('"unit_of_meas":"°C"', 1, true)
  and (find("grenton/CLU100000001/ONS0001/state") or {}).p == "21.5")
local timer = config("sensor", "TMR0001")
check("Timer -> remaining duration sensor", timer and timer.p:find('"dev_cla":"duration"', 1, true)
  and timer.p:find('"unit_of_meas":"s"', 1, true)
  and math.tointeger(tonumber((find("grenton/CLU100000001/TMR0001/state") or {}).p)) ~= nil
  and tonumber((find("grenton/CLU100000001/TMR0001/state") or {}).p) > 50
  and tonumber((find("grenton/CLU100000001/TMR0001/state") or {}).p) <= 60)
local timer_pubs = count("grenton/CLU100000001/TMR0001/state")
run(20) -- 2 s of 100 ms scans while the timer counts
check("running Timer: at most one state per second",
      count("grenton/CLU100000001/TMR0001/state") - timer_pubs <= 3)
check("Timer -> Start/Stop buttons named after the object",
  (config("button", "TMR0001_2") or {}).p:find('"name":"TMR0001 start"', 1, true) ~= nil
  and (config("button", "TMR0001_3") or {}).p:find('"name":"TMR0001 stop"', 1, true) ~= nil)
local sched = config("sensor", "SCH0001")
check("Scheduler -> value sensor", sched and (find("grenton/CLU100000001/SCH0001/state") or {}).p == "42")
check("Scheduler -> running switch", (config("switch", "SCH0001_2") or {}).p:find('"name":"SCH0001 running"', 1, true) ~= nil)
local thermo = config("climate", "THE0001")
check("Thermostat -> climate discovery", thermo and thermo.p:find('"curr_temp_t":"~/state"', 1, true)
  and thermo.p:find('"temp_cmd_t":"~/temperature/set"', 1, true)
  and thermo.p:find('"mode_cmd_t":"~/mode/set"', 1, true)
  and thermo.p:find('"pr_mode_cmd_t":"~/preset/set"', 1, true))
local thermo_state = __mqtt.jdecode((find("grenton/CLU100000001/THE0001/state") or {}).p or "")
check("Thermostat state from interface features", thermo_state and thermo_state.temperature == 21.5
  and thermo_state.current_temperature == 20.3 and thermo_state.action == "heating")
local presence = config("binary_sensor", "PRE0001")
check("PresenceSensor -> occupancy", presence and presence.p:find('"dev_cla":"occupancy"', 1, true))
check("PresenceSensor -> locked switch", (config("switch", "PRE0001_2") or {}).p:find('"name":"PRE0001 locked"', 1, true) ~= nil)
local mysensors_input = config("binary_sensor", "MSI0001")
check("MySensors module: MySensors manufacturer; not on the bus yet -> CLU availability only", mysensors_input
  and mysensors_input.node.dev.mf == "MySensors" and mysensors_input.node.avty == nil)
local hub = config("sensor", "HUB0001")
check("ONE_WIRE_HUB -> active sensors (diagnostic) + refresh button", hub and hub.p:find('"name":"HUB0001 active sensors"', 1, true)
  and hub.p:find('"ent_cat":"diagnostic"', 1, true)
  and (config("button", "HUB0001_2") or {}).p:find('"ent_cat":"config"', 1, true) ~= nil)
local panel = config("event", "PAN0001_event")
check("SMART_PANEL PANEL: own device under the panel, event named after it", panel
  and panel.t == DEVICE_CFG .. NODE .. "_PAN0001/config" and panel.node.dev.name == "PAN0001"
  and panel.node.dev.via_device == NODE .. "_30000001" and not panel.p:find('"name"', 1, true))
check("SMART_PANEL PANEL -> gesture / proximity / display event entity", panel and panel.p:find(
  '"evt_typ":["gesture_up","gesture_down","gesture_left","gesture_right","proximity","page_change","display_on","display_off"]', 1, true)
  and not panel.p:find('"dev_cla"', 1, true))
check("Calendar -> State switch, next run sensor, event", config("switch", "CAL0001") ~= nil
  and (config("sensor", "CAL0001_2") or {}).p:find('"name":"CAL0001 next run"', 1, true) ~= nil
  and (config("event", "CAL0001_event") or {}).p:find('"evt_typ":["calendar"]', 1, true) ~= nil)
local sunrise = config("sensor", "SUN0001")
check("SunriseSunset -> sunrise / sunset timestamps, day binary_sensor", sunrise
  and sunrise.p:find('"dev_cla":"timestamp"', 1, true) and sunrise.p:find('"name":"SUN0001 sunrise"', 1, true)
  and config("sensor", "SUN0001_2") ~= nil and config("binary_sensor", "SUN0001_3") ~= nil)
check("SunriseSunset timestamp state", ((find("grenton/CLU100000001/SUN0001/state") or {}).p or ""):match("^%d+%-%d+%-%d+T%d+:%d+:%d+%+00:00$") ~= nil)
check("Statistics -> value sensors", (find("grenton/CLU100000001/STA0001/state") or {}).p == "0"
  and (config("sensor", "PUL0001") or {}).p:find('"stat_cla":"total_increasing"', 1, true) ~= nil)
check("SMART_PANEL PANEL -> Display On / Beep buttons",
  (config("button", "PAN0001_2") or {}).p:find('"name":"Display On"', 1, true) ~= nil
  and (config("button", "PAN0001_3") or {}).p:find('"name":"Beep"', 1, true) ~= nil)
check("SMART_PANEL PANEL -> free draw entities",
  (config("button", "PAN0001_4") or {}).p:find('"name":"Display Clear"', 1, true) ~= nil
  and (config("text", "PAN0001_5") or {}).p:find('"max":16', 1, true) ~= nil
  and (config("number", "PAN0001_6") or {}).p:find('"max":127', 1, true) ~= nil
  and (config("number", "PAN0001_7") or {}).p:find('"max":63', 1, true) ~= nil
  and (config("select", "PAN0001_8") or {}).p:find('"ops":["Small","Medium","Large"]', 1, true) ~= nil
  and (config("button", "PAN0001_9") or {}).p:find('"name":"Display Show Buttons"', 1, true) ~= nil
  and (find("grenton/CLU100000001/PAN0001_6/state") or {}).p == "0"
  and (find("grenton/CLU100000001/PAN0001_8/state") or {}).p == "Small")
local panel_button = config("event", "PBT0001_event")
check("PANEL_BUTTON: own device under the panel", panel_button
  and panel_button.t == DEVICE_CFG .. NODE .. "_PBT0001/config"
  and encoded(panel_button.node.dev) == '{"ids":["opengr8on_CLU100000001_PBT0001"],"mf":"Grenton","name":"PBT0001","via_device":"opengr8on_CLU100000001_30000001"}')
check("PANEL_BUTTON device: available with the panel", panel_button and panel_button.node.avty_mode == "all"
  and panel_button.node.avty[2].t == "grenton/CLU100000001/system/tfbus/modules/30000001")
check("PANEL_BUTTON -> event named after the device (null), LED on / off buttons",
  panel_button and not panel_button.p:find('"name"', 1, true)
  and (config("button", "PBT0001_2") or {}).p:find('"name":"LED Green On"', 1, true) ~= nil
  and (config("button", "PBT0001_3") or {}).p:find('"name":"LED Red On"', 1, true) ~= nil
  and (config("button", "PBT0001_4") or {}).p:find('"name":"LED Off"', 1, true) ~= nil
  and (config("button", "PBT0001_5") or {}).p:find('"name":"LED Green Blink"', 1, true) ~= nil
  and (config("button", "PBT0001_6") or {}).p:find('"name":"LED Red Blink"', 1, true) ~= nil)
local label_cfg, mode_cfg = config("text", "PBT0001_7"), config("select", "PBT0001_8")
check("PANEL_BUTTON -> Label text (max 15), Mode select (config)", label_cfg and mode_cfg
  and label_cfg.p:find('"max":15', 1, true) and label_cfg.p:find('"ent_cat":"config"', 1, true)
  and mode_cfg.p:find('"ops":["Monostable","Bistable","Locked"]', 1, true) and mode_cfg.p:find('"name":"Mode"', 1, true)
  and (find("grenton/CLU100000001/PBT0001_8/state") or {}).p == "Monostable")
local icon_a_cfg, icon_b_cfg = config("text", "PBT0001_9"), config("text", "PBT0001_10")
check("PANEL_BUTTON -> Icon A / Icon B text (max 15, config)", icon_a_cfg and icon_b_cfg
  and icon_a_cfg.p:find('"name":"Icon A"', 1, true) and icon_b_cfg.p:find('"name":"Icon B"', 1, true)
  and icon_a_cfg.p:find('"max":15', 1, true) and icon_b_cfg.p:find('"ent_cat":"config"', 1, true))
local number_feature = config("number", "var_restored")
check("user features: own config of the CLU device", number_feature
  and number_feature.t == DEVICE_CFG .. NODE .. "_var/config" and number_feature.node.dev.name == "CLU TEST")
check("number user feature -> number entity under var/", number_feature
  and number_feature.p:find('"~":"grenton/CLU100000001/var/restored"', 1, true) and number_feature.p:find('"mode":"box"', 1, true)
  and number_feature.p:find('"min":-2147483648', 1, true) and number_feature.p:find('"name":"restored"', 1, true)
  and (find("grenton/CLU100000001/var/restored/state") or {}).p == "0")
check("string user feature -> text entity", (config("text", "var_keep") or {}).p:find('"max":255', 1, true) ~= nil
  and (find("grenton/CLU100000001/var/keep/state") or {}).p == "init")
check("boolean user feature -> switch", config("switch", "var_flag") ~= nil
  and (find("grenton/CLU100000001/var/flag/state") or {}).p == "ON")
check("switch state ON", (find("grenton/CLU100000001/DOU0001/state") or {}).p == "ON")
check("state retained", (find("grenton/CLU100000001/DOU0001/state") or {}).r == true)
check("binary_sensor OFF", (find("grenton/CLU100000001/DIN0001/state") or {}).p == "OFF")
check("cover stopped", (find("grenton/CLU100000001/ROL0001/state") or {}).p == "stopped")
check("cover position", (find("grenton/CLU100000001/ROL0001/position") or {}).p == "55")
TMR0001:execute(1)
run(2)

-- ---- internals --------------------------------------------------------------------
local st = config("sensor", "system_clu_state")
check("CLU state enum sensor", st and st.p:find('"dev_cla":"enum"', 1, true) and st.p:find('"ops":["emergency","ok","starting"]', 1, true)
  and st.p:find('"ent_cat":"diagnostic"', 1, true) and not st.p:find("stat_cla", 1, true))
check("CLU state value", (find("grenton/CLU100000001/system/clu/state") or {}).p == "ok")
local boot = config("sensor", "system_clu_boot_time")
check("CLU boot time sensor", boot and boot.p:find('"dev_cla":"timestamp"', 1, true) ~= nil)
check("CLU boot time value", ((find("grenton/CLU100000001/system/clu/boot_time") or {}).p or ""):match("^%d+%-%d+%-%d+T%d+:%d+:%d+%+00:00$") ~= nil)
check("CLU time_change event entity", config("event", "system_clu_event") ~= nil)
check("no CLU<sn>/CLU<sn>* entity topics", count("grenton/CLU100000001/CLU100000001") == 0)
check("leaf sensors: state_topic is ~", (config("sensor", "system_memory_heap_free") or {}).p:find('"stat_t":"~"', 1, true) ~= nil
  and (config("sensor", "system_memory_heap_free") or {}).p:find('"~":"grenton/CLU100000001/system/memory/heap_free"', 1, true) ~= nil)
check("no flat system_* / module topics", count("grenton/CLU100000001/system_") == 0 and count("grenton/CLU100000001/13ab6c81") == 0)
check("tfbus counters", (find("grenton/CLU100000001/system/tfbus/online") or {}).p ~= nil
  and (find("grenton/CLU100000001/system/tfbus/total") or {}).p ~= nil)
check("tfbus load / busy in %", (find("grenton/CLU100000001/system/tfbus/load") or {}).p == "3.4"
  and (find("grenton/CLU100000001/system/tfbus/busy") or {}).p == "62.5"
  and (config("sensor", "system_tfbus_load") or {}).p:find('"unit_of_meas":"%"', 1, true) ~= nil)
check("CPU load per core in %", (find("grenton/CLU100000001/system/cpu/load_0") or {}).p == "12.5"
  and (find("grenton/CLU100000001/system/cpu/load_1") or {}).p == "40.5"
  and (config("sensor", "system_cpu_load_1") or {}).p:find('"unit_of_meas":"%"', 1, true) ~= nil)
local link_cfg = config("binary_sensor", "system_tfbus_modules_13ab6c81")
check("module link entity in the module's device config", link_cfg and link_cfg.t == DEVICE_CFG .. NODE .. "_13ab6c81/config"
  and link_cfg.p:find('"name":"TF-Bus link"', 1, true) ~= nil)
check("module link: CLU availability only (overrides the device's link availability)", link_cfg
  and encoded(__mqtt.jdecode(link_cfg.p).avty) == '[{"t":"grenton/CLU100000001/status"}]')
local heap_cfg = config("sensor", "system_memory_heap_free")
check("firmware internals: own config of the CLU device", heap_cfg
  and heap_cfg.t == DEVICE_CFG .. NODE .. "_system/config" and heap_cfg.node.dev.name == "CLU TEST")
check("local CLU object (state, boot time): in the CLU's objects config",
  (config("sensor", "system_clu_state") or {}).t == DEVICE_CFG .. NODE .. "/config")
check("module connectivity", (find("grenton/CLU100000001/system/tfbus/modules/13ab6c81") or {}).p == "ON"
  and (find("grenton/CLU100000001/system/tfbus/modules/20c85581") or {}).p == "OFF"
  and (config("binary_sensor", "system_tfbus_modules_13ab6c81") or {}).p:find('"dev_cla":"connectivity"', 1, true) ~= nil
  and (config("binary_sensor", "system_tfbus_modules_13ab6c81") or {}).p:find('"name":"TF-Bus link"', 1, true) ~= nil)
check("heap in KiB", (find("grenton/CLU100000001/system/memory/heap_free") or {}).p == "200")
check("rssi", (find("grenton/CLU100000001/system/network/rssi") or {}).p == "-61")
check("ip text sensor", (find("grenton/CLU100000001/system/network/ip") or {}).p == "192.168.1.100"
  and not (config("sensor", "system_network_ip") or {}).p:find("stat_cla", 1, true))
check("reset reason", (find("grenton/CLU100000001/system/clu/reset_reason") or {}).p == "task_watchdog")
check("firmware version sensor (diagnostic)", (find("grenton/CLU100000001/system/clu/firmware") or {}).p == "v1.4.0-3-gabcdef"
  and (config("sensor", "system_clu_firmware") or {}).p:find('"ent_cat":"diagnostic"', 1, true) ~= nil)
check("last crash text sensor", (find("grenton/CLU100000001/system/clu/last_crash") or {}).p
  == "task watchdog: clu_lua (CPU 1) | task IDLE1 | pc 0x400d1234"
  and (config("sensor", "system_clu_last_crash") or {}).p:find('"name":"Last crash"', 1, true) ~= nil)
check("lua memory", tonumber((find("grenton/CLU100000001/system/memory/lua") or {}).p) ~= nil)
local rb = config("button", "system_clu_restart")
check("restart button", rb and rb.p:find('"cmd_t":"~/set"', 1, true) and rb.p:find('"dev_cla":"restart"', 1, true)
  and rb.p:find('"ent_cat":"config"', 1, true))
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

local function command(topic, payload)
  inbox = { { "grenton/CLU100000001/" .. topic, payload } }
  run(1)
end
command("TMR0001_2/set", "PRESS")
check("Timer Start button", TMR0001:get(2) == 1)
command("TMR0001_3/set", "PRESS")
check("Timer Stop button", TMR0001:get(2) == 0)
command("SCH0001_2/set", "ON")
check("Scheduler switch starts", SCH0001:get(1) == 1)
command("SCH0001_2/set", "OFF")
check("Scheduler switch stops", SCH0001:get(1) == 0)
command("PRE0001_2/set", "ON")
check("Presence Locked switch on", PRE0001:get(5) == 1)
command("PRE0001_2/set", "OFF")
check("Presence Locked switch off", PRE0001:get(5) == 0)
command("THE0001/temperature/set", "22.26")
check("Thermostat temperature rounds to half-degree", THE0001:get(3) == 22.5)
command("THE0001/mode/set", "off")
check("Thermostat mode off stops", THE0001:get(6) == 0)
command("THE0001/mode/set", "heat")
check("Thermostat mode heat starts", THE0001:get(6) == 1)
command("THE0001/preset/set", "away")
check("Thermostat away preset", THE0001:get(8) == 1)
command("THE0001/preset/set", "none")
check("Thermostat none exits holiday", THE0001:get(8) == 0)
command("THE0001/preset/set", "auto")
check("Thermostat auto preset", THE0001:get(8) == 2)
command("THE0001/preset/set", "none")
command("THE0001/preset/set", "boost")
check("Thermostat boost preset", THE0001:get(8) == 3 and THE0001._heatup.add == 5)
command("var/restored/set", "12.5")
check("number user feature set", restored == 12.5)
command("var/restored/set", "abc")
check("number user feature: non-number ignored", restored == 12.5)
command("var/keep/set", " Hello World ")
check("text user feature set verbatim (trimmed, case kept)", keep == "Hello World")
command("var/flag/set", "OFF")
check("boolean user feature off", flag == false)
command("var/flag/set", "ON")
check("boolean user feature on", flag == true)
command("HUB0001_2/set", "PRESS")
executed = {}
for button_index = 2, 6 do command("PBT0001_" .. button_index .. "/set", "PRESS") end
command("PAN0001_2/set", "PRESS")
command("PAN0001_3/set", "PRESS")
-- free draw: each entity wakes, draws and flushes in one go; clear and print are separate
local panel_calls, panel_execute = {}, PAN0001.execute
PAN0001.execute = function(_, method_index, ...)
  local call = { method_index }
  for _, argument in ipairs({ ... }) do call[#call + 1] = tostring(argument) end
  panel_calls[#panel_calls + 1] = table.concat(call, ",")
end
command("PAN0001_6/set", "10")
command("PAN0001_7/set", "99")    -- clamped to 63
command("PAN0001_8/set", "Large")
check("free draw settings kept, no panel call", #panel_calls == 0)
command("PAN0001_4/set", "PRESS")
check("Display Clear -> SwitchOnDisplay, ClearScreen, DisplayContent", table.concat(panel_calls, " ") == "0 2 7")
panel_calls = {}
command("PAN0001_5/set", "Hello World")
check("Display Text -> SwitchOnDisplay, PrintText(X, Y, text, font), DisplayContent (no clear)",
  table.concat(panel_calls, " ") == "0 3,10,63,Hello World,3 7")
run(2)
check("Display Text / X / Y / Font states", (find("grenton/CLU100000001/PAN0001_5/state") or {}).p == "Hello World"
  and (find("grenton/CLU100000001/PAN0001_6/state") or {}).p == "10"
  and (find("grenton/CLU100000001/PAN0001_7/state") or {}).p == "63"
  and (find("grenton/CLU100000001/PAN0001_8/state") or {}).p == "Large")
module_statuses[0x30000001].fw_api_ver = 4
panel_calls = {}
command("PAN0001_4/set", "PRESS")
check("panel with fwApiVer < 5: flushed with Draw", table.concat(panel_calls, " ") == "0 2 29")
module_statuses[0x30000001].fw_api_ver = nil
panel_calls = {}
command("PAN0001_9/set", "PRESS")
check("Display Show Buttons -> ShowButtons", table.concat(panel_calls, " ") == "1")
panel_calls = {}
command("PAN0001_10/set", "3")
command("PAN0001_10/set", "9") -- clamped to 4
command("PAN0001_11/set", "PRESS")
command("PAN0001_12/set", "PRESS")
check("Display Page -> SetPageNr, Next / Previous Page buttons", table.concat(panel_calls, " ") == "22,3 22,4 27 28")
run(2)
check("Display Page state: page last set", (find("grenton/CLU100000001/PAN0001_10/state") or {}).p == "4")
PAN0001.execute = panel_execute
command("PBT0001_7/set", "Kitchen light over 15")
check("Label text -> Label feature, cut to 15", PBT0001:get(4) == "Kitchen light o")
command("PBT0001_9/set", "light_on")
command("PBT0001_10/set", "light_off_and_more")
check("Icon A / Icon B text -> IconA / IconB features, cut to 15",
  PBT0001:get(5) == "light_on" and PBT0001:get(6) == "light_off_and_m")
command("PBT0001_8/set", "Locked")
command("PBT0001_8/set", "locked") -- not an option: ignored
check("Mode select -> Mode feature (case kept)", PBT0001:get(1) == 2)
run(2)
check("Mode select state follows", (find("grenton/CLU100000001/PBT0001_8/state") or {}).p == "Locked")
check("panel buttons -> LED methods with OM's LED_CMD byte, SwitchOnDisplay, SetBeep",
  table.concat(executed, " ") == "PBT0001:1(2) PBT0001:2(16) PBT0001:2(4) PBT0001:0(0) PBT0001:0(1) PAN0001:0 PAN0001:21")
command("CAL0001/set", "OFF")
check("Calendar switch off -> Stop", CAL0001:get(3) == 0)

-- ---- lights (JSON schema) -------------------------------------------------------------
local calls = {}
local function rec(o)
  function o.execute(self, idx, ...) calls[#calls + 1] = { self._name, idx, ... } end
end
rec(DIM0001); rec(LED0001)
local function light(name, p) inbox = { { "grenton/CLU100000001/" .. name .. "/set", p } }; calls = {}; run(1) end
local function call(i) return table.concat((function(c) local t = {} for k = 1, 5 do t[k] = tostring(c and c[k]) end return t end)(calls[i]), ",") end
light("DIM0001", '{"state":"ON","brightness":64}')
check("DIMM brightness -> Value", #calls == 0 and math.abs(DIM0001._f[0] - 64 / 255) < 1e-9)
run(2)
check("DIMM state follows", (find("grenton/CLU100000001/DIM0001/state") or {}).p == '{"brightness":64,"color_mode":"brightness","state":"ON"}')
light("DIM0001", '{"state":"OFF","transition":1.5}')
check("DIMM OFF -> SwitchOff(0, 1500)", call(1) == "DIM0001,3,0,1500,nil")
DIM0001._f[1] = 700
light("DIM0001", '{"state":"ON"}')
check("DIMM ON -> SwitchOn(0, RampTime, LastValue)", call(1) == "DIM0001,2,0,700,1")
light("DIM0001", "off")
check("DIMM plain OFF", call(1) == "DIM0001,3,0,nil,nil")
light("DIM0001", '{"brightness":10}')
check("DIMM: no state -> ignored", #calls == 0)

light("LED0001", '{"state":"ON","color":{"r":255,"g":0,"b":0,"w":255},"brightness":128}')
check("LEDRGB colour + brightness", call(1) == "LED0001,6,#800000,nil,nil" and call(2) == "LED0001,12,128,nil,nil")
light("LED0001", '{"state":"ON","brightness":100}')
check("LEDRGB brightness keeps colour", call(1) == "LED0001,6,#643200,nil,nil" and call(2) == "LED0001,12,25,nil,nil")
light("LED0001", '{"state":"ON","color":{"r":0,"g":0,"b":255,"w":0},"transition":2}')
check("LEDRGB colour keeps brightness", call(1) == "LED0001,6,#0000c8,2000,nil" and call(2) == "LED0001,12,0,2000,nil")
light("LED0001", '{"state":"ON"}')
check("LEDRGB ON while on: nothing", #calls == 0)
light("LED0001", '{"state":"OFF"}')
check("LEDRGB OFF -> SwitchOff", call(1) == "LED0001,10,0,nil,nil")
LED0001._f[3], LED0001._f[4], LED0001._f[5], LED0001._f[15] = 0, 0, 0, 0
run(2)
check("LEDRGB OFF state", (find("grenton/CLU100000001/LED0001/state") or {}).p == '{"color_mode":"rgbw","state":"OFF"}')
light("LED0001", '{"state":"ON"}')
check("LEDRGB ON restores last colour", call(1) == "LED0001,6,#c86400,nil,nil" and call(2) == "LED0001,12,50,nil,nil")
light("LED0001", "bogus")
check("LEDRGB bad payload ignored", #calls == 0)

-- LED_CHANNEL: one lamp per channel
rec(LCH0001)
light("LCH0001", '{"state":"ON","brightness":40,"transition":0.5}')
check("LED_CHANNEL brightness -> SetValue", call(1) == "LCH0001,0,40,500,nil" and #calls == 1)
light("LCH0001", '{"state":"OFF"}')
check("LED_CHANNEL OFF -> SwitchOff", call(1) == "LCH0001,2,0,nil,nil")
light("LCH0001", "ON")
check("LED_CHANNEL ON -> SwitchOn(0, Ramp, LastValue)", call(1) == "LCH0001,1,0,nil,1")

-- LED_CCT
rec(CCT0001)
light("CCT0001", '{"state":"ON","color_temp":3000}')
check("LED_CCT ON + colour temp while off -> SetColorTemp + SwitchOn", call(1) == "CCT0001,0,3000,nil,nil" and call(2) == "CCT0001,4,0,nil,1")
CCT0001._f[2] = 100
light("CCT0001", '{"state":"ON","color_temp":9000,"transition":1}')
check("LED_CCT colour temp while on (clamped)", call(1) == "CCT0001,0,6500,1000,nil" and #calls == 1)
light("CCT0001", '{"state":"ON","brightness":30,"color_temp":2700}')
check("LED_CCT both", call(1) == "CCT0001,0,2700,nil,nil" and call(2) == "CCT0001,2,30,nil,nil")
light("CCT0001", "OFF")
check("LED_CCT OFF -> SwitchOff", call(1) == "CCT0001,5,0,nil,nil")
run(2)
check("LED_CCT state (on)", (find("grenton/CLU100000001/CCT0001/state") or {}).p == '{"brightness":100,"color_mode":"color_temp","color_temp":4600,"state":"ON"}')
command("LED0001_3/set", "20000")
command("LED0001_4/set", "80")
check("LEDRGB Ramp Time (clamped) / Red Correction -> features", LED0001:get(7) == 15000 and LED0001:get(10) == 80)
command("CCT0001_3/set", "500")
check("LED_CCT Ramp Time -> feature 5", CCT0001:get(5) == 500)

-- ---- events -----------------------------------------------------------------------
__fire("DIN0001", 6)
run(1)
local e = find("grenton/CLU100000001/DIN0001/event")
check("button click event", e ~= nil and e.p == '{"event_type":"click"}' and e.r == false)
__fire("LED0001", 5); __fire("CCT0001", 1)
run(1)
check("LEDRGB OnOutOfRange / LED_CCT OnColorTempChange events",
  (find("grenton/CLU100000001/LED0001/event") or {}).p == '{"event_type":"out_of_range"}'
  and (find("grenton/CLU100000001/CCT0001/event") or {}).p == '{"event_type":"color_temp_change"}')
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

-- ---- module versions reported after the build: configs resent with them ------------------
module_statuses[0x20c85581].hw_ver, module_statuses[0x20c85581].fw_ver = 1, "3.1.0"
pubs = {}
run(20) -- scan notices the versions, rebuild, then every config drains (PUBS_PER_TICK per tick)
check("late VERSION_QUERY: module configs resent with the versions",
  (config("cover", "ROL0001") or { node = { dev = {} } }).node.dev.sw == "3.1.0"
  and (config("cover", "ROL0001") or { node = { dev = {} } }).node.dev.hw == "1")
pubs = {}
run(20)
check("versions unchanged: configs not resent again", count(CFG) == 0)

-- ---- a module the bus master finds after the build: link availability added -------------------
module_statuses[0x0a000004] = { online = true, fails = 0 }
pubs = {}
run(20)
local found_module = config("binary_sensor", "MSI0001")
check("module found later: its config gets the link availability", found_module and found_module.node.avty
  and found_module.node.avty[2].t == "grenton/CLU100000001/system/tfbus/modules/0a000004")
pubs = {}
run(20)
check("found module: configs not resent again", count(CFG) == 0)

-- ---- HA restart -------------------------------------------------------------------
local before = count(CFG)
inbox = { { "homeassistant/status", "online" } }
run(3)
check("HA online -> discovery resent", count(CFG) > before)

-- ---- remap at runtime: removed components and devices -------------------------------
local MODULE_CFG = DEVICE_CFG .. NODE .. "_13ab6c81/config"
MQTT.hide(DOU0001)
MQTT.expose(DIN0001, "switch") -- binary_sensor + event -> switch
pubs = {}
run(10)
local removed = config("switch", "DOU0001")
check("hidden entity: removal stub once, in its device's config", removed and removed.p == '{"p":"switch"}'
  and removed.r == true and removed.t == MODULE_CFG)
check("remapped object: new platform's component, old ones removed",
  (config("switch", "DIN0001") or {}).p:find('"uniq_id":"opengr8on_CLU100000001_DIN0001"', 1, true) ~= nil
  and (config("binary_sensor", "DIN0001") or {}).p == '{"p":"binary_sensor"}'
  and (config("event", "DIN0001_event") or {}).p == '{"p":"event"}')
__persist.flush(true)
local f = io.open(storage, "rb")
local saved = f and f:read("a") or ""
if f then f:close() end
check("published components persisted per device config", saved:find('["' .. MODULE_CFG .. '"]', 1, true) ~= nil
  and saved:find('["switch_DIN0001"]="switch"', 1, true) ~= nil and saved:find('["switch_DOU0001"]', 1, true) == nil)
pubs = {}
inbox = { { "homeassistant/status", "online" } }
run(10)
check("removal stubs sent only once", (find(MODULE_CFG) or {}).p ~= nil
  and not find(MODULE_CFG).p:find('"switch_DOU0001"', 1, true))
MQTT.hide("system")
pubs = {}
run(10)
check("device config without entities cleared", (find(DEVICE_CFG .. NODE .. "_system/config") or {}).p == ""
  and find(DEVICE_CFG .. NODE .. "_system/config").r == true)

-- ---- reconnect -> full republish ----------------------------------------------------
connected = false
run(2)
pubs = {}
connected, gen = true, 2
run()
check("reconnect republishes states", find("grenton/CLU100000001/ROL0001/position") ~= nil)
check("reconnect republishes discovery", config("cover", "ROL0001") ~= nil)
check("reconnect: hidden device not resent", find(DEVICE_CFG .. NODE .. "_system/config") == nil)

-- ---- early scan: commands and events publish before the periodic scan -----------------
local function step(n)
  for _ = 1, n or 1 do
    clock = clock + 10
    SYSTEM.Loop()
  end
end
local DOU_STATE = "grenton/CLU100000001/DOU0002/state"
run(2) -- periodic scan just ran: the next one is 100 ms away
pubs = {}
DOU0002._f[0] = 0
step(2)
check("change without event waits for the periodic scan", find(DOU_STATE) == nil)
step(8)
check("periodic scan publishes it", (find(DOU_STATE) or {}).p == "OFF")
pubs = {}
DOU0002._f[0] = 1
__fire("DOU0002", 0)
step(2)
check("event of a stateful object: state published at once", (find(DOU_STATE) or {}).p == "ON")
pubs = {}
DOU0002._f[0] = 0
__fire("DOU0002", 0)
step(1)
check("early scans are spaced SCAN_MIN_MS apart", find(DOU_STATE) == nil)
step(1)
check("next early scan after SCAN_MIN_MS", (find(DOU_STATE) or {}).p == "OFF")
run(2)
pubs = {}
inbox[#inbox + 1] = { "grenton/CLU100000001/DOU0002/set", "OFF" }
step(2)
check("command: state republished at once", (find(DOU_STATE) or {}).p == "OFF")

-- ---- script API / JSON ---------------------------------------------------------------
check("MQTT.connected", MQTT.connected() == true)
check("MQTT.publish", MQTT.publish("x/y", 5, true) and pubs[#pubs].t == "x/y" and pubs[#pubs].p == "5")
check("json escaping", __mqtt.json({ a = 'q"\\\n\1' }) == '{"a":"q\\"\\\\\\n\\u0001"}')
check("expose rejects unknown kind", not pcall(MQTT.expose, DOU0001, "toaster"))

os.remove(storage)
os.remove((storage:gsub("%.%w+$", "")) .. ".TMP")
print(failures == 0 and "all ok" or (failures .. " failure(s)"))
os.exit(failures == 0 and 0 or 1)
