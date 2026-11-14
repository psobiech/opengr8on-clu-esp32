-- @file mqtt_util.lua
-- MQTT state/commands + Home Assistant discovery. Loaded after objects.lua (component clu_mqtt).
-- C side (clu_mqtt.c): connection, LWT "<root>/status", subscriptions, inbound queue. Here:
--   * entities: objects declared by OM.LUA, mapped per class (DEFAULTS) or by USER.LUA overrides
--   * discovery: retained "<prefix>/<component>/opengr8on_CLU<sn>/<id>/config", resent on every
--     (re)connect and on Home Assistant's "online"; configs of entities that disappeared are cleared
--   * state: polled every scan_ms, published retained on change only
--   * commands: "<root>/<obj>/set" and kind-specific subtopics -> fixed object calls (never eval)
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
local clu = __clu
local cfg = clu.mqtt_cfg()
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
local function feat(object, index)
  local succeeded, value = pcall(object.get, object, index)
  if succeeded then return value end
end

local function onoff(value)
  if value == nil then return nil end
  return (tonumber(value) or 0) ~= 0 and "ON" or "OFF"
end

local function num(value)
  value = tonumber(value)
  if not value or value ~= value then return nil end
  if math.type(value) == "integer" then return tostring(value) end
  return string.format("%.6g", value)
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

local function on_off_kind(component)
  return function(object, options)
    local feature = options.feature or 0
    -- DOUT outputs switch through SwitchOn/SwitchOff (the driver drives the module and keeps the
    -- timed-switch state right); anything else gets set(feature, 1|0).
    local direct = object._class == CLS_DOUT and not options.feature
    return {
      comp = component,
      disc = { state_topic = "~/state", command_topic = "~/set" },
      states = { state = function() return onoff(feat(object, feature)) end },
      cmds = {
        set = function(payload)
          local is_on = TRUE[payload] and true or FALSE[payload] and false
          if is_on == nil then return end
          if options.on ~= nil and options.off ~= nil then object:execute(is_on and options.on or options.off)
          elseif direct then object:execute(is_on and 1 or 2)
          else object:set(feature, is_on and 1 or 0) end
        end,
      },
    }
  end
end
