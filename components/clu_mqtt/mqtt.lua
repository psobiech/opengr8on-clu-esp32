-- MQTT state/commands + Home Assistant discovery. Loaded after objects.lua (component clu_mqtt).
-- C side (clu_mqtt.c): connection, LWT "<root>/status", subscriptions, inbound queue. Here:
--   * entities: objects declared by OM.LUA, mapped per class (DEFAULTS) or by USER.LUA overrides
--   * discovery: retained "<prefix>/<component>/opengr8on_CLU<sn>/<id>/config", resent on every
--     (re)connect and on Home Assistant's "online"; configs of entities that disappeared are cleared
--   * state: polled every scan_ms, published retained on change only
--   * commands: "<root>/<obj>/set", "<root>/<obj>/position/set" -> fixed object calls (never eval)
--   * internals (menuconfig CLU_MQTT_DIAGNOSTICS) under "<root>/system/", values published
--     directly on the topic (no "/state"): clu/{boot_time,state,reset_reason,event,restart/set},
--     memory/{heap_free,heap_min,psram_free,lua}, network/{rssi,ip},
--     tfbus/{online,total,missed_polls}, tfbus/modules/<module>; noisy ones sampled every diag_ms
--   * raw object API (menuconfig CLU_MQTT_OBJECT_API): get/set any feature, call any method with
--     JSON payloads (see api_message below)
-- Friendly names come from OM.LUA's "-- NAME_IO <label>=<object>" comments.
--
-- USER.LUA API (works before or after OM.LUA declares the objects):
--   MQTT.expose(obj | "NAME", "light")                      replace the default mapping
--   MQTT.expose(TIM1234, { kind = "sensor", feature = 3, unit = "ms", name = "Countdown" })
--   MQTT.expose(TIM1234, { kind = "button", execute = 0, name = "Start" })
--   MQTT.hide(obj | "NAME" | "system")
--   MQTT.publish(topic, payload [, retain])   MQTT.connected()   MQTT.root
local C = __clu
local cfg = C.mqtt_cfg()
local objects = __objects
local persist = __persist
local HOOKS = __hooks

local ROOT = cfg.root
local NODE = "opengr8on_CLU" .. cfg.sn
local OUTBOX_MAX = 8 * 1024  -- bytes in esp-mqtt's outbox before we stop feeding it this tick
local PUBS_PER_TICK = 16
local EVENTS_MAX = 64         -- pending non-coalesced messages (events, API replies) in the queue
local RECV_PER_TICK = 8

local json = __json.encode -- objects.lua

-- ---------------------------------------------------------------------------------------------
-- Value helpers
-- ---------------------------------------------------------------------------------------------
local function feat(o, idx)
  local ok, v = pcall(o.get, o, idx)
  if ok then return v end
end

local function onoff(v)
  if v == nil then return nil end
  return (tonumber(v) or 0) ~= 0 and "ON" or "OFF"
end

local function num(v)
  v = tonumber(v)
  if not v or v ~= v then return nil end
  if math.type(v) == "integer" then return tostring(v) end
  return string.format("%.6g", v)
end

local TRUE = { ON = true, ["1"] = true, TRUE = true }
local FALSE = { OFF = true, ["0"] = true, FALSE = true }

-- ---------------------------------------------------------------------------------------------
-- Entity kinds: kind(o, opts) -> { comp, disc = {...}, states = {key = fn}, cmds = {sub = fn},
-- events = {idx = "type"} }. Topics are relative to "~" = <root>/<object>.
-- ---------------------------------------------------------------------------------------------
local CLS_DOUT, CLS_ROLLER = 4, 24
local BUTTON_EVENTS = { [1] = "press", [2] = "release", [3] = "short_press", [4] = "long_press",
                        [5] = "hold", [6] = "click" }

local function on_off_kind(comp)
  return function(o, opts)
    local f = opts.feature or 0
    -- DOUT outputs switch through SwitchOn/SwitchOff (the driver drives the module and keeps the
    -- timed-switch state right); anything else gets set(feature, 1|0).
    local direct = o._class == CLS_DOUT and not opts.feature
    return {
      comp = comp,
      disc = { state_topic = "~/state", command_topic = "~/set" },
      states = { state = function() return onoff(feat(o, f)) end },
      cmds = {
        set = function(p)
          local on = TRUE[p] and true or FALSE[p] and false
          if on == nil then return end
          if direct then o:execute(on and 1 or 2) else o:set(f, on and 1 or 0) end
        end,
      },
    }
  end
end

-- ---------------------------------------------------------------------------------------------
-- Dimmable lights: HA JSON schema (state/command payload {"state":"ON","brightness":0-255,
-- "color":{"r","g","b","w"},"transition":s}). Plain ON / OFF commands are accepted too.
-- ---------------------------------------------------------------------------------------------
local CLS_DIMM, CLS_LEDRGB = 5, 9
local jdecode = __json.decode -- objects.lua

local function b255(x)
  x = math.floor((tonumber(x) or 0) + 0.5)
  return x < 0 and 0 or x > 255 and 255 or x
end

-- Command payload -> table with .state "ON" / "OFF" (nil = not a light command).
local function light_cmd(p)
  local c = jdecode(p)
  if type(c) ~= "table" then
    p = p:upper()
    c = { state = TRUE[p] and "ON" or FALSE[p] and "OFF" or nil }
  end
  if type(c.state) == "string" then c.state = c.state:upper() end
  if c.state ~= "ON" and c.state ~= "OFF" then return nil end
  return c
end

-- "transition" [s] -> ramp [ms] (nil = the object's RampTime).
local function ramp_ms(c)
  local t = tonumber(c.transition)
  if t and t >= 0 then return math.floor(t * 1000 + 0.5) end
end

local function light_disc(mode)
  return { schema = "json", state_topic = "~/state", command_topic = "~/set", brightness = true,
           supported_color_modes = { mode } }
end

-- DIMM: feature 0 Value (0..1) <-> brightness. ON without brightness = SwitchOn with Mode
-- LastValue, OFF = SwitchOff (both take the transition); brightness = set Value (RampTime).
local function dimm_light(o)
  return {
    comp = "light",
    raw = true,
    disc = light_disc("brightness"),
    states = {
      state = function()
        local v = tonumber(feat(o, 0)) or 0
        if v <= 0 then return json({ state = "OFF", color_mode = "brightness" }) end
        return json({ state = "ON", color_mode = "brightness", brightness = math.max(1, b255(v * 255)) })
      end,
    },
    cmds = {
      set = function(p)
        local c = light_cmd(p)
        if not c then return end
        local ramp = ramp_ms(c)
        if c.state == "OFF" then
          o:execute(3, 0, ramp)                                     -- SwitchOff(Time, Ramp)
        elseif tonumber(c.brightness) then
          o:set(0, b255(c.brightness) / 255)
        else
          o:execute(2, 0, ramp or tonumber(feat(o, 1)), 1)        -- SwitchOn(Time, Ramp, Mode)
        end
      end,
    },
  }
end

-- LEDRGB: the output is R/G/B (features 3-5, already scaled by Value) + W (15), 0-255 each.
-- HA sees brightness = the brightest channel and color = the output scaled up to that; commands
-- go out as SetRGBvalue("#rrggbb", Ramp) + SetWhiteValue(w, Ramp). ON without a colour restores
-- the last lit colour (SwitchOn before there was one), OFF = SwitchOff.
local function rgbw_light(o)
  local last -- last lit output {r, g, b, w}
  local function output()
    return { b255(feat(o, 3)), b255(feat(o, 4)), b255(feat(o, 5)), b255(feat(o, 15)) }
  end
  local function peak(c) return math.max(c[1], c[2], c[3], c[4]) end
  return {
    comp = "light",
    raw = true,
    disc = light_disc("rgbw"),
    states = {
      state = function()
        local out = output()
        local m = peak(out)
        if m == 0 then return json({ state = "OFF", color_mode = "rgbw" }) end
        last = out
        return json({ state = "ON", color_mode = "rgbw", brightness = m,
                      color = { r = b255(out[1] * 255 / m), g = b255(out[2] * 255 / m),
                                b = b255(out[3] * 255 / m), w = b255(out[4] * 255 / m) } })
      end,
    },
    cmds = {
      set = function(p)
        local c = light_cmd(p)
        if not c then return end
        local ramp = ramp_ms(c)
        if c.state == "OFF" then o:execute(10, 0, ramp); return end -- SwitchOff(Time, Ramp)
        local out = output()
        local m = peak(out)
        local base = m > 0 and out or last
        local col = type(c.color) == "table" and c.color
        if not col and not tonumber(c.brightness) then
          if m > 0 then return end
          if not base then o:execute(9, 0, ramp); return end       -- SwitchOn(Time, Ramp)
        end
        local bm = base and peak(base) or 0
        local rgbw
        if col then
          rgbw = { b255(col.r), b255(col.g), b255(col.b), b255(col.w) }
        elseif bm > 0 then
          rgbw = { base[1] * 255 / bm, base[2] * 255 / bm, base[3] * 255 / bm, base[4] * 255 / bm }
        else
          rgbw = { 255, 255, 255, 0 }
        end
        local br = tonumber(c.brightness) and b255(c.brightness) or (bm > 0 and bm) or 255
        for i = 1, 4 do rgbw[i] = b255(rgbw[i] * br / 255) end
        o:execute(6, string.format("#%02x%02x%02x", rgbw[1], rgbw[2], rgbw[3]), ramp) -- SetRGBvalue
        o:execute(12, rgbw[4], ramp)                                                   -- SetWhiteValue
      end,
    },
  }
end

-- LED_CHANNEL (one channel of LED_RGBW, e.g. a separate lamp) and LED_CCT (warm + cold channel
-- pair): Value (0-255) = brightness. brightness = SetValue(v, Ramp), ON without brightness =
-- SwitchOn with Mode LastValue, OFF = SwitchOff; all take the transition. LED_CCT also maps
-- ColorTemp (2700-6500 K, HA in kelvin) <-> SetColorTemp(K, Ramp).
local CLS_LEDCH, CLS_LEDCCT = 49, 72
local LEDCH = { value = 0, set = 0, on = 1, off = 2 }               -- feature / method indices
local LEDCCT = { value = 2, set = 2, on = 4, off = 5, ct = 0, set_ct = 0 }
local CT_MIN, CT_MAX = 2700, 6500

local function channel_light(o, ix)
  local mode = ix.ct and "color_temp" or "brightness"
  local disc = light_disc(mode)
  if ix.ct then
    disc.color_temp_kelvin, disc.min_kelvin, disc.max_kelvin = true, CT_MIN, CT_MAX
  end
  return {
    comp = "light",
    raw = true,
    disc = disc,
    states = {
      state = function()
        local v = b255(feat(o, ix.value))
        local st = { state = v > 0 and "ON" or "OFF", color_mode = mode }
        if v > 0 then st.brightness = v end
        if ix.ct then st.color_temp = math.tointeger(tonumber(feat(o, ix.ct))) end
        return json(st)
      end,
    },
    cmds = {
      set = function(p)
        local c = light_cmd(p)
        if not c then return end
        local ramp = ramp_ms(c)
        if c.state == "OFF" then o:execute(ix.off, 0, ramp); return end -- SwitchOff(Time, Ramp)
        local k = ix.ct and tonumber(c.color_temp)
        if k then
          k = math.floor(math.max(CT_MIN, math.min(CT_MAX, k)) + 0.5)
          o:execute(ix.set_ct, k, ramp)                                   -- SetColorTemp(K, Ramp)
        end
        if tonumber(c.brightness) then
          o:execute(ix.set, b255(c.brightness), ramp)                     -- SetValue(Value, Ramp)
        elseif not k or b255(feat(o, ix.value)) == 0 then
          o:execute(ix.on, 0, ramp, 1)                                    -- SwitchOn(Time, Ramp, Mode)
        end
      end,
    },
  }
end

local KINDS = {}
KINDS.switch = on_off_kind("switch")
local onoff_light = on_off_kind("light")
-- light: brightness for DIMM / LED_CHANNEL, + colour temperature for LED_CCT, RGBW for LEDRGB,
-- on/off for anything else (or with opts.feature).
KINDS.light = function(o, opts)
  if not opts.feature then
    if o._class == CLS_DIMM then return dimm_light(o) end
    if o._class == CLS_LEDRGB then return rgbw_light(o) end
    if o._class == CLS_LEDCH then return channel_light(o, LEDCH) end
    if o._class == CLS_LEDCCT then return channel_light(o, LEDCCT) end
  end
  return onoff_light(o, opts)
end

KINDS.binary_sensor = function(o, opts)
  local f = opts.feature or 0
  return {
    comp = "binary_sensor",
    disc = { state_topic = "~/state" },
    states = { state = function() return onoff(feat(o, f)) end },
  }
end

-- opts.fn(v) transforms the raw value, opts.map = {value = "text"} makes an enum sensor,
-- opts.text = true publishes the value as a string. Non-numeric sensors get no state_class.
KINDS.sensor = function(o, opts)
  local f = opts.feature or 0
  local options, numeric = nil, not (opts.text or opts.map or opts.device_class == "timestamp")
  if opts.map then
    local seen = {}
    options = {}
    for _, t in pairs(opts.map) do
      if not seen[t] then options[#options + 1] = t; seen[t] = true end
    end
    table.sort(options)
  end
  return {
    comp = "sensor",
    disc = { state_topic = "~/state", unit_of_measurement = opts.unit, options = options,
             device_class = opts.map and "enum" or nil,
             state_class = numeric and (opts.state_class or "measurement") or nil },
    states = {
      state = function()
        local v = feat(o, f)
        if opts.fn and v ~= nil then v = opts.fn(v) end
        if v == nil then return nil end
        if opts.map then return opts.map[v] or tostring(v) end
        if not numeric then return tostring(v) end
        return num(v)
      end,
    },
  }
end

-- Button: payload PRESS calls opts.press(o) or o:execute(opts.execute).
KINDS.button = function(o, opts)
  return {
    comp = "button",
    disc = { command_topic = "~/set" },
    cmds = {
      set = function(p)
        if p ~= "PRESS" then return end
        if opts.press then opts.press(o) elseif opts.execute then o:execute(opts.execute) end
      end,
    },
  }
end

-- TFBUS module (class 2) link state, from the bus master.
KINDS.module = function(o)
  return {
    comp = "binary_sensor",
    default_name = o._sn and string.format("Module %d", o._sn),
    disc = { state_topic = "~/state", device_class = "connectivity" },
    states = {
      state = function()
        local m = C.tfbus_module and o._sn and C.tfbus_module(o._sn)
        if m then return m.online and "ON" or "OFF" end
      end,
    },
  }
end

-- ROLLER_SHUTTER: feature 0 State (0 stop, 1 up, 2 down), 7 Position (0 closed .. 100 open, = HA).
-- OPEN/CLOSE use HoldUp/HoldDown (always move that way, even when moving the other way; the module
-- stops at the end of travel), STOP = Stop, position = SetPosition.
KINDS.cover = function(o)
  return {
    comp = "cover",
    disc = { state_topic = "~/state", command_topic = "~/set", position_topic = "~/position",
             set_position_topic = "~/position/set", state_stopped = "stopped",
             device_class = "shutter" },
    states = {
      state = function()
        local s = feat(o, 0)
        return s == 1 and "opening" or s == 2 and "closing" or "stopped"
      end,
      position = function() return num(feat(o, 7)) end,
    },
    cmds = {
      set = function(p)
        if p == "OPEN" then o:execute(5) elseif p == "CLOSE" then o:execute(6)
        elseif p == "STOP" then o:execute(3) end
      end,
      ["position/set"] = function(p)
        local n = tonumber(p)
        if n then o:execute(10, math.max(0, math.min(100, math.floor(n + 0.5)))) end
      end,
    },
  }
end

-- Inputs/buttons: Grenton events (OnSwitchOn, OnClick, ...) as an HA event entity.
KINDS.event = function(o, opts)
  local map = opts.events or BUTTON_EVENTS
  local dc = opts.device_class -- false = none
  if dc == nil then dc = "button" end
  local types, seen = {}, {}
  for i = 0, 63 do
    local t = map[i]
    if t and not seen[t] then types[#types + 1] = t; seen[t] = true end
  end
  return {
    comp = "event",
    suffix = "event",
    topic = "event",
    disc = { state_topic = "~/event", event_types = types, device_class = dc or nil },
    events = map,
  }
end

-- Default mapping per object class (from the module interfaces).
local DEFAULTS = {
  [3] = { { kind = "binary_sensor" }, { kind = "event" } },                         -- DIN
  [4] = { { kind = "switch" } },                                                    -- DOUT
  [5] = { { kind = "light" } },                                                     -- DIMM
  [7] = { { kind = "event" } },                                                     -- BUTTON
  [9] = { { kind = "light" } },                                                     -- LEDRGB
  [20] = { { kind = "sensor", device_class = "temperature", unit = "°C" } },        -- PANELSENSTEMP
  [21] = { { kind = "sensor", unit = "%", icon = "mdi:brightness-5" } },            -- PANELSENSLIGHT
  [24] = { { kind = "cover" } },                                                    -- ROLLER_SHUTTER
  [30] = { { kind = "event" } },                                                    -- PANEL_BUTTON
  [49] = { { kind = "light" } },                                                    -- LED_CHANNEL
  [72] = { { kind = "light" } },                                                    -- LED_CCT
}

-- ---------------------------------------------------------------------------------------------
-- Internals (cfg.diag_ms set = menuconfig CLU_MQTT_DIAGNOSTICS): local CLU object, TFBUS modules
-- and firmware values from C.sys_info(). All in HA's "Diagnostic" / "Configuration" sections.
-- ---------------------------------------------------------------------------------------------
local DIAG = "diagnostic"
local SYS_NAME = "system" -- MQTT.hide("system") hides the firmware entities

-- Boot time from Uptime: stable timestamp (republished only when it moves > 60 s, i.e. on a
-- reboot or a clock step), unknown until the clock is set.
local boot_epoch
local function boot_time(uptime)
  local now = os.time()
  if now < 1600000000 then return nil end
  local b = now - (tonumber(uptime) or 0)
  if not boot_epoch or math.abs(b - boot_epoch) > 60 then boot_epoch = b end
  return os.date("!%Y-%m-%dT%H:%M:%S+00:00", boot_epoch)
end

local function kib(v) return (tonumber(v) or 0) // 1024 end

local sys_cache, sys_t = {}, nil
local SYSOBJ = { _name = SYS_NAME, _class = -1 }
function SYSOBJ.get(_, key)
  local now = C.millis()
  if not sys_t or now - sys_t >= 1000 then
    sys_cache = C.sys_info and C.sys_info() or {}
    sys_cache.lua_mem = math.floor(collectgarbage("count"))
    sys_t = now
  end
  return sys_cache[key]
end

-- Topic tree under <root>/system/ (group/base). Sensors are leaves: the value is the topic itself.
local SYSTEM_ENTITIES = {
  { kind = "sensor", group = "system/memory", base = "heap_free", feature = "heap_free",
    name = "Free heap", unit = "KiB", device_class = "data_size", fn = kib, sampled = true },
  { kind = "sensor", group = "system/memory", base = "heap_min", feature = "heap_min",
    name = "Minimum free heap", unit = "KiB", device_class = "data_size", fn = kib, sampled = true },
  { kind = "sensor", group = "system/memory", base = "psram_free", feature = "psram_free",
    name = "Free PSRAM", unit = "KiB", device_class = "data_size", fn = kib, sampled = true },
  { kind = "sensor", group = "system/memory", base = "lua", feature = "lua_mem",
    name = "Lua memory", unit = "KiB", device_class = "data_size", sampled = true },
  { kind = "sensor", group = "system/network", base = "rssi", feature = "rssi",
    name = "WiFi signal", unit = "dBm", device_class = "signal_strength", sampled = true },
  { kind = "sensor", group = "system/network", base = "ip", feature = "ip", name = "IP address",
    text = true, icon = "mdi:ip-network" },
  { kind = "sensor", group = "system/clu", base = "reset_reason", feature = "reset_reason",
    name = "Last reset", text = true, icon = "mdi:restart-alert" },
  { kind = "sensor", group = "system/tfbus", base = "online", feature = "tfbus_online",
    name = "TFBUS modules online", icon = "mdi:lan-connect" },
  { kind = "sensor", group = "system/tfbus", base = "total", feature = "tfbus_total",
    name = "TFBUS modules", icon = "mdi:lan" },
  { kind = "sensor", group = "system/tfbus", base = "missed_polls", feature = "tfbus_fails",
    name = "TFBUS missed polls", state_class = "total_increasing", icon = "mdi:lan-disconnect",
    sampled = true },
  { kind = "button", group = "system/clu", base = "restart", name = "Restart",
    device_class = "restart", entity_category = "config", press = function() C.sys_restart() end },
}

if cfg.diag_ms then
  DEFAULTS[0] = {                                                                   -- local CLU
    { kind = "sensor", group = "system/clu", base = "boot_time", leaf = true, feature = 0,
      name = "Boot time", device_class = "timestamp", fn = boot_time, entity_category = DIAG },
    { kind = "sensor", group = "system/clu", base = "state", leaf = true, feature = 2,
      name = "CLU state", entity_category = DIAG,
      map = { [0] = "starting", [1] = "ok", [4] = "emergency" } },
    { kind = "event", base = "system/clu", name = "CLU", events = { [13] = "time_change" },
      device_class = false, entity_category = DIAG },                               -- system/clu/event
  }
  if C.gate then table.remove(DEFAULTS[0], 2) end -- GATE: feature 2 is PrimaryDNS, there is no State
  DEFAULTS[2] = { { kind = "module", group = "system/tfbus/modules", leaf = true,   -- TFBUS module
                    entity_category = DIAG } }
end

-- ---------------------------------------------------------------------------------------------
-- Entity registry
-- ---------------------------------------------------------------------------------------------
local overrides = {}  -- object name -> list of opts | false (hidden)
local entities = {}   -- built list
local by_base = {}    -- topic path under root ("DOU1234", "DOU1234_2", "system/clu/restart") -> entity
local by_object = {}  -- object name -> entities with events
local labels, clu_label = nil, nil
local ready, dirty = false, true

local function load_labels()
  labels = {}
  for _, fn in ipairs({ "OM.LUA", "om.lua" }) do
    local f = io.open((C.root or ".") .. "/" .. fn, "rb")
    if f then
      for line in f:lines() do
        local kind, label, id = line:match("^%s*%-%-%s*NAME_(%u+)%s+(.-)=([%w_]+)%s*$")
        if kind then
          label = label:gsub("_", " ")
          labels[id] = label
          if kind == "CLU" then clu_label = label end
        end
      end
      f:close()
      break
    end
  end
end

local device, origin

local function build()
  if not labels then load_labels() end
  device = {
    identifiers = { NODE },
    name = clu_label or ("CLU" .. cfg.sn),
    manufacturer = "OpenGr8on",
    model = "ESP32-S3 CLU",
    serial_number = tostring(cfg.sn),
    sw_version = cfg.fw,
  }
  origin = { name = "opengr8on-clu-esp32" }
  entities, by_base, by_object = {}, {}, {}

  local function add(name, o, list)
    for n, opts in ipairs(list or {}) do
      local kind = KINDS[opts.kind]
      if not kind then
        C.log(2, "mqtt: " .. name .. ": unknown kind " .. tostring(opts.kind))
      else
        local e = kind(o, opts)
        e.obj = o
        -- e.base: topic path under root. First state-bearing entity of an object owns
        -- <root>/<obj>; later ones get <obj>_<n>; opts.group prefixes a path ("system/memory").
        local base = opts.base or name
        if not opts.base and n > 1 and not e.topic then base = name .. "_" .. n end
        if opts.group then base = opts.group .. "/" .. base end
        e.base = base
        e.id = base:gsub("/", "_") .. (e.suffix and ("_" .. e.suffix) or "")
        e.uid = NODE .. "_" .. e.id
        -- Leaf: the single state is published on <root>/<base> itself, not <root>/<base>/state.
        if opts.leaf and e.states and e.states.state then
          e.states[""], e.states.state = e.states.state, nil
          e.disc.state_topic = "~"
        end
        e.name = opts.name or labels[name] or e.default_name or name
        if opts.device_class then e.disc.device_class = opts.device_class end
        if opts.icon then e.disc.icon = opts.icon end
        if opts.entity_category then e.disc.entity_category = opts.entity_category end
        if opts.sampled then e.every = cfg.diag_ms end -- noisy values: sampled, not scanned
        entities[#entities + 1] = e
        if e.cmds then by_base[e.base] = e end
        if e.events then
          local l = by_object[name] or {}
          l[#l + 1] = e
          by_object[name] = l
        end
      end
    end
  end

  local names = {}
  for name in pairs(objects) do names[#names + 1] = name end
  table.sort(names)
  for _, name in ipairs(names) do
    local list = overrides[name]
    if list == nil then list = DEFAULTS[objects[name]._class] end
    add(name, objects[name], list)
  end

  if cfg.diag_ms and overrides[SYS_NAME] ~= false then
    local list = {}
    for _, opts in ipairs(SYSTEM_ENTITIES) do
      opts.entity_category = opts.entity_category or DIAG
      if opts.leaf == nil then opts.leaf = opts.kind ~= "button" end
      list[#list + 1] = opts
    end
    for _, opts in ipairs(overrides[SYS_NAME] or {}) do list[#list + 1] = opts end
    add(SYS_NAME, SYSOBJ, list)
  end
  dirty = false
end

local function state_topic(e, key)
  return ROOT .. "/" .. e.base .. (key == "" and "" or "/" .. key)
end

local function config_topic(e) return cfg.prefix .. "/" .. e.comp .. "/" .. NODE .. "/" .. e.id .. "/config" end

local function config_payload(e)
  local d = { ["~"] = ROOT .. "/" .. e.base, name = e.name, unique_id = e.uid,
              availability_topic = cfg.status, device = device, origin = origin }
  for k, v in pairs(e.disc) do d[k] = v end
  return json(d)
end

-- ---------------------------------------------------------------------------------------------
-- Outbound queue: paced into esp-mqtt's outbox. Retained states coalesce per topic (only the
-- latest value of a pending topic is sent), so they are bounded by the topic count. Events (and
-- API replies) never coalesce: at most EVENTS_MAX of them wait, newer ones are dropped (and
-- counted) while the queue is full, so a publisher that cannot keep up never exhausts the heap.
-- ---------------------------------------------------------------------------------------------
local q, qhead, qtail, qidx = {}, 1, 0, {}
local qev, dropped = 0, 0 -- pending non-coalesced entries; events dropped in this overflow
local last = {}       -- topic -> last published retained state

-- Forget what was published: every state goes out again on the next scan (sampled ones too).
local function republish()
  last = {}
  for _, e in ipairs(entities) do e.due = nil end
end
local up, gen = false, nil

local function q_reset()
  q, qhead, qtail, qidx, qev = {}, 1, 0, {}, 0
end

local function enqueue(topic, payload, retain, qos, coalesce)
  if coalesce then
    local e = qidx[topic]
    if e then e.p = payload; return end
  elseif qev >= EVENTS_MAX then
    if dropped == 0 then C.log(2, "mqtt: event queue full, dropping new events") end
    dropped = dropped + 1
    return
  else
    qev = qev + 1
    if dropped > 0 then
      C.log(2, "mqtt: " .. dropped .. " event(s) dropped")
      dropped = 0
    end
  end
  local e = { t = topic, p = payload, r = retain, qos = qos, c = coalesce }
  qtail = qtail + 1
  q[qtail] = e
  if coalesce then qidx[topic] = e end
end

local function flush()
  local n = 0
  while qhead <= qtail and n < PUBS_PER_TICK and C.mqtt_outbox() < OUTBOX_MAX do
    local e = q[qhead]
    q[qhead] = nil -- free sent entries now: under steady load the queue may never fully drain
    qhead = qhead + 1
    if e.c then
      if qidx[e.t] == e then qidx[e.t] = nil end
    else
      qev = qev - 1
    end
    if not C.mqtt_publish(e.t, e.p, e.r, e.qos) then
      q_reset() -- link dropped: everything is resent on the next connect
      return
    end
    n = n + 1
  end
  if qhead > qtail then q_reset() end
end

local function publish_discovery()
  if not cfg.ha then return end
  local mine = persist.section("mqtt")
  local now = {}
  for _, e in ipairs(entities) do
    local t = config_topic(e)
    now[#now + 1] = t
    enqueue(t, config_payload(e), true, 1, true)
  end
  -- Clear configs this CLU published before but no longer has (object removed / hidden / remapped).
  local keep = {}
  for _, t in ipairs(now) do keep[t] = true end
  for _, t in ipairs(mine.configs or {}) do
    if not keep[t] then enqueue(t, "", true, 1, true) end
  end
  table.sort(now)
  local changed = #now ~= #(mine.configs or {})
  for i, t in ipairs(now) do if not changed and mine.configs[i] ~= t then changed = true end end
  if changed then
    mine.configs = now
    persist.mark()
  end
end

local function scan(now)
  for _, e in ipairs(entities) do
    local due = true
    if e.every then
      due = now >= (e.due or 0)
      if due then e.due = now + e.every end
    end
    if e.states and due then
      for key, fn in pairs(e.states) do
        local ok, v = pcall(fn)
        if ok and v ~= nil then
          local t = state_topic(e, key)
          if last[t] ~= v then
            last[t] = v
            enqueue(t, v, true, 0, true)
          end
        end
      end
    end
  end
end

-- Events: OBJECT._fire is the one path every Grenton event takes (driver events via __fire and
-- virtual objects calling o:_fire), so wrap it.
local orig_fire = OBJECT._fire
function OBJECT:_fire(idx)
  orig_fire(self, idx)
  if not up then return end
  local l = by_object[self._name]
  if not l then return end
  for _, e in ipairs(l) do
    local t = e.events[idx]
    if t then enqueue(ROOT .. "/" .. e.base .. "/event", json({ event_type = t }), false, 0, false) end
  end
end

-- ---------------------------------------------------------------------------------------------
-- Raw object API (cfg.api = menuconfig CLU_MQTT_OBJECT_API). Payloads are JSON values, decoded
-- here (never evaluated as Lua):
--   <root>/<OBJ>/feature/<idx>/get   -> <root>/<OBJ>/feature/<idx>  {"ok":true,"value":v}
--   <root>/<OBJ>/feature/<idx>/set   v -> same reply with the value read back
--   <root>/<OBJ>/execute/<idx>       [args] | v | "" -> <root>/<OBJ>/execute/<idx>/result
-- Remote-CLU proxies (class 1) are refused: their execute(0, s) runs s as Lua on the peer.
-- ---------------------------------------------------------------------------------------------
local function api_reply(topic, ok, v)
  local body = ok and ('{"ok":true,"value":' .. json(v) .. "}") or ('{"ok":false,"error":' .. json(tostring(v)) .. "}")
  enqueue(topic, body, false, 0, false)
end

local function api_target(name, idx)
  local o = objects[name]
  if not o then return nil, "unknown object " .. name end
  if o._class == 1 then return nil, "remote CLU objects are not reachable over MQTT" end
  idx = math.tointeger(tonumber(idx))
  if not idx or idx < 0 or idx > 255 then return nil, "bad index" end
  return o, idx
end

-- Payload -> value: JSON when it parses, else the text itself (so `set` accepts plain words).
local function api_value(payload)
  local v, err = jdecode(payload)
  if err then return payload end
  return v
end

local function api_message(rest, payload)
  local name, idx, op = rest:match("^([^/]+)/feature/(%d+)/(%a+)$")
  if name and (op == "get" or op == "set") then
    local reply = ROOT .. "/" .. name .. "/feature/" .. idx
    local o, i = api_target(name, idx)
    if not o then api_reply(reply, false, i); return name end
    if op == "set" then
      local v = api_value(payload)
      if type(v) == "table" then api_reply(reply, false, "value must be a scalar"); return name end
      local ok, err = pcall(o.set, o, i, v)
      if not ok then api_reply(reply, false, err); return name end
    end
    api_reply(reply, pcall(o.get, o, i))
    return name
  end
  name, idx = rest:match("^([^/]+)/execute/(%d+)$")
  if name then
    local reply = ROOT .. "/" .. name .. "/execute/" .. idx .. "/result"
    local o, i = api_target(name, idx)
    if not o then api_reply(reply, false, i); return name end
    local args, n = {}, 0
    if payload:match("%S") then
      local v = api_value(payload)
      if type(v) == "table" and v.n then args, n = v, v.n
      elseif type(v) == "table" then api_reply(reply, false, "args must be a JSON array"); return name
      else args, n = { v }, 1 end
    end
    api_reply(reply, pcall(o.execute, o, i, table.unpack(args, 1, n)))
    return name
  end
  return false
end

local CMD_SUBS = { "position/set", "set" } -- longest first

local function on_message(topic, payload)
  if cfg.ha and topic == cfg.prefix .. "/status" then
    if payload == "online" then
      -- Home Assistant restarted: resend configs and all states.
      republish()
      publish_discovery()
    end
    return
  end
  if topic:sub(1, #ROOT + 1) ~= ROOT .. "/" then return end
  if cfg.api then
    local target = api_message(topic:sub(#ROOT + 2), payload)
    if target ~= false then
      -- The object may have changed: resend its entity states on the next scan.
      for _, e in ipairs(entities) do
        if e.obj == objects[target] then
          for key in pairs(e.states or {}) do last[state_topic(e, key)] = nil end
        end
      end
      return
    end
  end
  -- <root>/<path>/<cmd>: path may have levels ("system/clu/restart"), cmd is "set" / "position/set".
  local rest, e, fn = topic:sub(#ROOT + 2), nil, nil
  for _, sub in ipairs(CMD_SUBS) do
    local base = rest:sub(-#sub - 1) == "/" .. sub and rest:sub(1, -#sub - 2)
    e = base and by_base[base]
    fn = e and e.cmds[sub]
    if fn then break end
  end
  if not fn then return end
  payload = payload:match("^%s*(.-)%s*$")
  if not e.raw then payload = payload:upper() end -- raw: JSON commands (lights)
  local ok, err = pcall(fn, payload)
  if not ok then C.log(1, "mqtt: " .. topic .. ": " .. tostring(err)) end
  -- Commands that change nothing (e.g. ON on an output that is on) would otherwise leave an
  -- optimistic HA UI out of step: republish the current state on the next scan.
  for key in pairs(e.states or {}) do last[state_topic(e, key)] = nil end
end

local next_scan = 0

local function tick(now)
  local is_up, g = C.mqtt_state()
  if not is_up then
    if up then q_reset(); republish() end
    up = false
    return
  end
  up = true
  if dirty and ready then
    build()
    if g == gen then publish_discovery() end
  end
  if g ~= gen then
    -- (Re)connected: the broker session is new, republish configs and every state.
    gen = g
    q_reset()
    republish()
    if ready then publish_discovery() end
  end
  for _ = 1, RECV_PER_TICK do
    local t, p = C.mqtt_recv()
    if not t then break end
    on_message(t, p)
  end
  if ready and now >= next_scan then
    next_scan = now + cfg.scan_ms
    scan(now)
  end
  flush()
end

HOOKS.init[#HOOKS.init + 1] = function() ready = true; dirty = true end
HOOKS.loop[#HOOKS.loop + 1] = tick

-- ---------------------------------------------------------------------------------------------
-- Script API
-- ---------------------------------------------------------------------------------------------
local function obj_name(o)
  if type(o) == "table" then return o._name end
  return tostring(o)
end

MQTT = { root = ROOT }

function MQTT.expose(o, opts)
  local name = obj_name(o)
  if type(opts) == "string" then opts = { kind = opts } end
  if type(opts) ~= "table" or not KINDS[opts.kind] then
    error("MQTT.expose: kind must be one of switch, light, binary_sensor, sensor, cover, event, button, module", 2)
  end
  local l = overrides[name]
  if not l then l = {}; overrides[name] = l end
  l[#l + 1] = opts
  dirty = true
end

function MQTT.hide(o)
  overrides[obj_name(o)] = false
  dirty = true
end

function MQTT.connected() return (C.mqtt_state()) end

function MQTT.publish(topic, payload, retain)
  return C.mqtt_publish(tostring(topic), tostring(payload), retain and true or false, 0)
end

__mqtt = { json = json, jdecode = jdecode, entities = function() return entities end } -- for tests / debugging
