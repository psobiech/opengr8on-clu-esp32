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
             tfbus_fails = sys_fails }
  end,
  tfbus_module = function(sn) return module_statuses[sn] end,
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
CAL0001 = OBJECT:new(15, "CAL0001")
SUN0001 = OBJECT:new(39, "SUN0001")
STA0001 = OBJECT:new(47, "STA0001")
PUL0001 = OBJECT:new(48, "PUL0001")
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
local NODE = "/opengr8on_CLU100000001/"

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
CLU100000001._f[0], CLU100000001._f[2] = 120, 1 -- Uptime, State (C hooks on the device)
connected, gen = true, 1
run()

local sw = find(CFG .. "switch" .. NODE .. "DOU0001/config")
check("DOU -> switch discovery", sw ~= nil and sw.r == true and sw.q == 1)
check("switch uses ~ base", sw and sw.p:find('"~":"grenton/CLU100000001/DOU0001"', 1, true) ~= nil)
check("friendly name from NAME_IO", sw and sw.p:find('"name":"LIGHT A"', 1, true) ~= nil)
check("module IO on its module device (model, serial, TF-Bus versions)", sw and sw.p:find(
  '"device":{"hw_version":"2","identifiers":["opengr8on_CLU100000001_13ab6c81"],"manufacturer":"Grenton","model":"IO MODULE DIN 8","name":"IO MODULE DIN 8 330001537","serial_number":"330001537","sw_version":"2.2.4","via_device":"opengr8on_CLU100000001"}', 1, true) ~= nil)
local tmr_cfg = find(CFG .. "sensor" .. NODE .. "TMR0001/config")
check("virtual object on the CLU device, named from NAME_CLU",
  tmr_cfg and tmr_cfg.p:find('"identifiers":["opengr8on_CLU100000001"]', 1, true) and tmr_cfg.p:find('"name":"CLU TEST"', 1, true) ~= nil)
local rol_cfg = find(CFG .. "cover" .. NODE .. "ROL0001/config")
check("other module: own device", rol_cfg and rol_cfg.p:find('"name":"ROLLER SH DIN 3 550000001"', 1, true) ~= nil)
check("TF-Bus module IO: available while the CLU is online and the module's link is ON", sw and sw.p:find(
  '"availability":[{"topic":"grenton/CLU100000001/status"},{"payload_available":"ON","payload_not_available":"OFF","topic":"grenton/CLU100000001/system/tfbus/modules/13ab6c81"}],"availability_mode":"all"', 1, true) ~= nil
  and not sw.p:find("availability_topic", 1, true))
check("CLU entity: CLU availability only", tmr_cfg and tmr_cfg.p:find('"availability_topic":"grenton/CLU100000001/status"', 1, true) ~= nil
  and not tmr_cfg.p:find('"availability":', 1, true))
check("CLU device: board model, chip, build version", tmr_cfg and tmr_cfg.p:find(
  '"device":{"hw_version":"esp32s3 v0.2","identifiers":["opengr8on_CLU100000001"],"manufacturer":"OpenGr8on","model":"Test board","name":"CLU TEST","serial_number":"100000001","sw_version":"v1.4.0-3-gabcdef"}', 1, true) ~= nil)
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
local dim = find(CFG .. "light" .. NODE .. "DIM0001/config")
check("DIMM -> brightness light", dim and dim.p:find('"schema":"json"', 1, true)
  and dim.p:find('"supported_color_modes":["brightness"]', 1, true) and dim.p:find('"command_topic":"~/set"', 1, true))
check("DIMM state", (find("grenton/CLU100000001/DIM0001/state") or {}).p == '{"brightness":128,"color_mode":"brightness","state":"ON"}')
local led = find(CFG .. "light" .. NODE .. "LED0001/config")
check("LEDRGB -> rgbw light", led and led.p:find('"supported_color_modes":["rgbw"]', 1, true))
local lch = find(CFG .. "light" .. NODE .. "LCH0001/config")
check("LED_CHANNEL -> brightness light", lch and lch.p:find('"supported_color_modes":["brightness"]', 1, true))
check("LED_CHANNEL state", (find("grenton/CLU100000001/LCH0001/state") or {}).p == '{"brightness":77,"color_mode":"brightness","state":"ON"}')
local cct = find(CFG .. "light" .. NODE .. "CCT0001/config")
check("LED_CCT -> color_temp light", cct and cct.p:find('"supported_color_modes":["color_temp"]', 1, true)
  and cct.p:find('"color_temp_kelvin":true', 1, true) and cct.p:find('"min_kelvin":2700', 1, true) and cct.p:find('"max_kelvin":6500', 1, true))
check("LED_CCT state (off)", (find("grenton/CLU100000001/CCT0001/state") or {}).p == '{"color_mode":"color_temp","color_temp":4600,"state":"OFF"}')
check("LEDRGB state", (find("grenton/CLU100000001/LED0001/state") or {}).p
  == '{"brightness":200,"color":{"b":0,"g":128,"r":255,"w":64},"color_mode":"rgbw","state":"ON"}')
check("no entity for POW/STO/remote CLU", find(CFG .. "switch" .. NODE .. "CLU100000002/config") == nil
  and count(CFG .. "sensor" .. NODE .. "STO") == 0
  and count("grenton/CLU100000001/CLU100000002") == 0)
local voltage = find(CFG .. "sensor" .. NODE .. "POW0001/config")
check("PowerSupplyVoltage -> voltage sensor", voltage and voltage.p:find('"device_class":"voltage"', 1, true)
  and voltage.p:find('"unit_of_measurement":"V"', 1, true)
  and (find("grenton/CLU100000001/POW0001/state") or {}).p == "23.8")
check("on-board input with hardware -> binary_sensor + event",
  find(CFG .. "binary_sensor" .. NODE .. "LDI0001/config") ~= nil and find(CFG .. "event" .. NODE .. "LDI0001_event/config") ~= nil)
check("on-board channels without hardware -> no entities",
  find(CFG .. "binary_sensor" .. NODE .. "LDI0002/config") == nil and find(CFG .. "event" .. NODE .. "LDI0002_event/config") == nil
  and find(CFG .. "switch" .. NODE .. "LDO0001/config") == nil and find(CFG .. "sensor" .. NODE .. "LPO0001/config") == nil
  and find(CFG .. "sensor" .. NODE .. "AIN0002/config") == nil
  and count("grenton/CLU100000001/LDO0001") == 0 and count("grenton/CLU100000001/LPO0001") == 0)
check("USER.LUA override exposes an on-board channel without hardware", find(CFG .. "switch" .. NODE .. "LDO0002/config") ~= nil)
local analog_in = find(CFG .. "sensor" .. NODE .. "AIN0001/config")
check("ANALOG_DIN AnalogIN -> sensor without voltage class / unit", analog_in
  and not analog_in.p:find('"device_class"', 1, true) and not analog_in.p:find('"unit_of_measurement"', 1, true)
  and analog_in.p:find('"name":"ANALOG DIN 167772162"', 1, true) ~= nil
  and (find("grenton/CLU100000001/AIN0001/state") or {}).p == "3.3")
local on_board_input = find(CFG .. "binary_sensor" .. NODE .. "LDI0001/config")
check("on-board module: emulated (OpenGr8on), no link availability", on_board_input
  and on_board_input.p:find('"manufacturer":"OpenGr8on"', 1, true) and not on_board_input.p:find('"availability":', 1, true))
local onewire = find(CFG .. "sensor" .. NODE .. "ONS0001/config")
check("ONEW_SENSOR -> temperature sensor", onewire and onewire.p:find('"device_class":"temperature"', 1, true)
  and onewire.p:find('"unit_of_measurement":"°C"', 1, true)
  and (find("grenton/CLU100000001/ONS0001/state") or {}).p == "21.5")
local timer = find(CFG .. "sensor" .. NODE .. "TMR0001/config")
check("Timer -> remaining duration sensor", timer and timer.p:find('"device_class":"duration"', 1, true)
  and timer.p:find('"unit_of_measurement":"s"', 1, true)
  and math.tointeger(tonumber((find("grenton/CLU100000001/TMR0001/state") or {}).p)) ~= nil
  and tonumber((find("grenton/CLU100000001/TMR0001/state") or {}).p) > 50
  and tonumber((find("grenton/CLU100000001/TMR0001/state") or {}).p) <= 60)
local timer_pubs = count("grenton/CLU100000001/TMR0001/state")
run(20) -- 2 s of 100 ms scans while the timer counts
check("running Timer: at most one state per second",
      count("grenton/CLU100000001/TMR0001/state") - timer_pubs <= 3)
check("Timer -> Start/Stop buttons", find(CFG .. "button" .. NODE .. "TMR0001_2/config") ~= nil
  and find(CFG .. "button" .. NODE .. "TMR0001_3/config") ~= nil)
local sched = find(CFG .. "sensor" .. NODE .. "SCH0001/config")
check("Scheduler -> value sensor", sched and (find("grenton/CLU100000001/SCH0001/state") or {}).p == "42")
check("Scheduler -> switch", find(CFG .. "switch" .. NODE .. "SCH0001_2/config") ~= nil)
local thermo = find(CFG .. "climate" .. NODE .. "THE0001/config")
check("Thermostat -> climate discovery", thermo and thermo.p:find('"current_temperature_topic":"~/state"', 1, true)
  and thermo.p:find('"temperature_command_topic":"~/temperature/set"', 1, true)
  and thermo.p:find('"mode_command_topic":"~/mode/set"', 1, true)
  and thermo.p:find('"preset_mode_command_topic":"~/preset/set"', 1, true))
local thermo_state = __mqtt.jdecode((find("grenton/CLU100000001/THE0001/state") or {}).p or "")
check("Thermostat state from interface features", thermo_state and thermo_state.temperature == 21.5
  and thermo_state.current_temperature == 20.3 and thermo_state.action == "heating")
local presence = find(CFG .. "binary_sensor" .. NODE .. "PRE0001/config")
check("PresenceSensor -> occupancy", presence and presence.p:find('"device_class":"occupancy"', 1, true))
check("PresenceSensor -> Locked switch", find(CFG .. "switch" .. NODE .. "PRE0001_2/config") ~= nil)
local hub = find(CFG .. "sensor" .. NODE .. "HUB0001/config")
check("ONE_WIRE_HUB -> active sensors (diagnostic) + refresh button", hub and hub.p:find('"name":"HUB0001 active sensors"', 1, true)
  and hub.p:find('"entity_category":"diagnostic"', 1, true)
  and (find(CFG .. "button" .. NODE .. "HUB0001_2/config") or {}).p:find('"entity_category":"config"', 1, true) ~= nil)
local panel = find(CFG .. "event" .. NODE .. "PAN0001_event/config")
check("SMART_PANEL PANEL -> gesture / proximity / display event entity", panel and panel.p:find(
  '"event_types":["gesture_up","gesture_down","gesture_left","gesture_right","proximity","page_change","display_on","display_off"]', 1, true)
  and not panel.p:find('"device_class"', 1, true))
check("Calendar -> State switch, next run sensor, event", find(CFG .. "switch" .. NODE .. "CAL0001/config") ~= nil
  and (find(CFG .. "sensor" .. NODE .. "CAL0001_2/config") or {}).p:find('"name":"CAL0001 next run"', 1, true) ~= nil
  and (find(CFG .. "event" .. NODE .. "CAL0001_event/config") or {}).p:find('"event_types":["calendar"]', 1, true) ~= nil)
local sunrise = find(CFG .. "sensor" .. NODE .. "SUN0001/config")
check("SunriseSunset -> sunrise / sunset timestamps, day binary_sensor", sunrise
  and sunrise.p:find('"device_class":"timestamp"', 1, true) and sunrise.p:find('"name":"SUN0001 sunrise"', 1, true)
  and find(CFG .. "sensor" .. NODE .. "SUN0001_2/config") ~= nil and find(CFG .. "binary_sensor" .. NODE .. "SUN0001_3/config") ~= nil)
check("SunriseSunset timestamp state", ((find("grenton/CLU100000001/SUN0001/state") or {}).p or ""):match("^%d+%-%d+%-%d+T%d+:%d+:%d+%+00:00$") ~= nil)
check("Statistics -> value sensors", (find("grenton/CLU100000001/STA0001/state") or {}).p == "0"
  and (find(CFG .. "sensor" .. NODE .. "PUL0001/config") or {}).p:find('"state_class":"total_increasing"', 1, true) ~= nil)
local number_feature = find(CFG .. "number" .. NODE .. "var_restored/config")
check("number user feature -> number entity under var/", number_feature
  and number_feature.p:find('"~":"grenton/CLU100000001/var/restored"', 1, true) and number_feature.p:find('"mode":"box"', 1, true)
  and number_feature.p:find('"min":-2147483648', 1, true) and number_feature.p:find('"name":"restored"', 1, true)
  and (find("grenton/CLU100000001/var/restored/state") or {}).p == "0")
check("string user feature -> text entity", (find(CFG .. "text" .. NODE .. "var_keep/config") or {}).p:find('"max":255', 1, true) ~= nil
  and (find("grenton/CLU100000001/var/keep/state") or {}).p == "init")
check("boolean user feature -> switch", find(CFG .. "switch" .. NODE .. "var_flag/config") ~= nil
  and (find("grenton/CLU100000001/var/flag/state") or {}).p == "ON")
check("switch state ON", (find("grenton/CLU100000001/DOU0001/state") or {}).p == "ON")
check("state retained", (find("grenton/CLU100000001/DOU0001/state") or {}).r == true)
check("binary_sensor OFF", (find("grenton/CLU100000001/DIN0001/state") or {}).p == "OFF")
check("cover stopped", (find("grenton/CLU100000001/ROL0001/state") or {}).p == "stopped")
check("cover position", (find("grenton/CLU100000001/ROL0001/position") or {}).p == "55")
TMR0001:execute(1)
run(2)

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
local link_cfg = find(CFG .. "binary_sensor" .. NODE .. "system_tfbus_modules_13ab6c81/config")
check("module link entity on the module device", link_cfg and link_cfg.p:find('"identifiers":["opengr8on_CLU100000001_13ab6c81"]', 1, true)
  and link_cfg.p:find('"name":"TF-Bus link"', 1, true) ~= nil)
check("module connectivity", (find("grenton/CLU100000001/system/tfbus/modules/13ab6c81") or {}).p == "ON"
  and (find("grenton/CLU100000001/system/tfbus/modules/20c85581") or {}).p == "OFF"
  and (find(CFG .. "binary_sensor" .. NODE .. "system_tfbus_modules_13ab6c81/config") or {}).p:find('"device_class":"connectivity"', 1, true) ~= nil
  and (find(CFG .. "binary_sensor" .. NODE .. "system_tfbus_modules_13ab6c81/config") or {}).p:find('"name":"TF-Bus link"', 1, true) ~= nil)
check("heap in KiB", (find("grenton/CLU100000001/system/memory/heap_free") or {}).p == "200")
check("rssi", (find("grenton/CLU100000001/system/network/rssi") or {}).p == "-61")
check("ip text sensor", (find("grenton/CLU100000001/system/network/ip") or {}).p == "192.168.1.100"
  and not (find(CFG .. "sensor" .. NODE .. "system_network_ip/config") or {}).p:find("state_class", 1, true))
check("reset reason", (find("grenton/CLU100000001/system/clu/reset_reason") or {}).p == "task_watchdog")
check("firmware version sensor (diagnostic)", (find("grenton/CLU100000001/system/clu/firmware") or {}).p == "v1.4.0-3-gabcdef"
  and (find(CFG .. "sensor" .. NODE .. "system_clu_firmware/config") or {}).p:find('"entity_category":"diagnostic"', 1, true) ~= nil)
check("last crash text sensor", (find("grenton/CLU100000001/system/clu/last_crash") or {}).p
  == "task watchdog: clu_lua (CPU 1) | task IDLE1 | pc 0x400d1234"
  and (find(CFG .. "sensor" .. NODE .. "system_clu_last_crash/config") or {}).p:find('"name":"Last crash"', 1, true) ~= nil)
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

-- ---- module versions reported after the build: configs resent with them ------------------
module_statuses[0x20c85581].hw_ver, module_statuses[0x20c85581].fw_ver = 1, "3.1.0"
pubs = {}
run(20) -- scan notices the versions, rebuild, then every config drains (PUBS_PER_TICK per tick)
check("late VERSION_QUERY: module configs resent with the versions",
  ((find(CFG .. "cover" .. NODE .. "ROL0001/config") or {}).p or ""):find('"sw_version":"3.1.0"', 1, true) ~= nil)
pubs = {}
run(20)
check("versions unchanged: configs not resent again", count(CFG) == 0)

-- ---- HA restart -------------------------------------------------------------------
local before = count(CFG)
inbox = { { "homeassistant/status", "online" } }
run(3)
check("HA online -> discovery resent", count(CFG) > before)

-- ---- remap at runtime: old configs cleared -----------------------------------------
MQTT.hide(DOU0001)
run(10)
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
