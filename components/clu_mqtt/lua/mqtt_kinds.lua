-- @file mqtt_kinds.lua
local KINDS = {}
KINDS.switch = on_off_kind("switch")
local onoff_light = on_off_kind("light")
-- light: brightness for DIMM / LED_CHANNEL, + colour temperature for LED_CCT, RGBW for LEDRGB,
-- on/off for anything else (or with opts.feature).
KINDS.light = function(object, options)
  if not options.feature then
    if object._class == CLS_DIMM then return dimm_light(object) end
    if object._class == CLS_LEDRGB then return rgbw_light(object) end
    if object._class == CLS_LEDCH then return channel_light(object, LEDCH) end
    if object._class == CLS_LEDCCT then return channel_light(object, LEDCCT) end
  end
  return onoff_light(object, options)
end

KINDS.binary_sensor = function(object, options)
  local feature = options.feature or 0
  return {
    comp = "binary_sensor",
    disc = { state_topic = "~/state" },
    states = { state = function() return onoff(feat(object, feature)) end },
  }
end

-- opts.fn(value) transforms the raw value, opts.map = {value = "text"} makes an enum sensor,
-- opts.text = true publishes the value as a string. Non-numeric sensors get no state_class.
KINDS.sensor = function(object, options)
  local feature = options.feature or 0
  local sensor_options, numeric = nil, not (options.text or options.map or options.device_class == "timestamp")
  if options.map then
    local seen = {}
    sensor_options = {}
    for _, option_text in pairs(options.map) do
      if not seen[option_text] then
        sensor_options[#sensor_options + 1] = option_text
        seen[option_text] = true
      end
    end
    table.sort(sensor_options)
  end
  return {
    comp = "sensor",
    disc = { state_topic = "~/state", unit_of_measurement = options.unit, options = sensor_options,
             device_class = options.map and "enum" or nil,
             state_class = numeric and (options.state_class or "measurement") or nil },
    states = {
      state = function()
        local raw_value = feat(object, feature)
        if options.fn and raw_value ~= nil then raw_value = options.fn(raw_value) end
        if raw_value == nil then return nil end
        if options.map then return options.map[raw_value] or tostring(raw_value) end
        if not numeric then return tostring(raw_value) end
        return num(raw_value)
      end,
    },
  }
end


-- Button: payload PRESS calls opts.press(o) or o:execute(opts.execute).
KINDS.button = function(object, options)
  return {
    comp = "button",
    disc = { command_topic = "~/set" },
    cmds = {
      set = function(payload)
        if payload ~= "PRESS" then return end
        if options.press then options.press(object)
        elseif options.execute then object:execute(options.execute) end
      end,
    },
  }
end

-- TFBUS module (class 2) link state, from the bus master.
KINDS.module = function(object)
  return {
    comp = "binary_sensor",
    default_name = object._sn and string.format("Module %d", object._sn),
    disc = { state_topic = "~/state", device_class = "connectivity" },
    states = {
      state = function()
        local module_status = clu.tfbus_module and object._sn and clu.tfbus_module(object._sn)
        if module_status then return module_status.online and "ON" or "OFF" end
      end,
    },
  }
end

-- ROLLER_SHUTTER: feature 0 State (0 stop, 1 up, 2 down), 7 Position (0 closed .. 100 open, = HA).
-- OPEN/CLOSE use HoldUp/HoldDown (always move that way, even when moving the other way; the module
-- stops at the end of travel), STOP = Stop, position = SetPosition.
KINDS.cover = function(object)
  return {
    comp = "cover",
    disc = { state_topic = "~/state", command_topic = "~/set", position_topic = "~/position",
             set_position_topic = "~/position/set", state_stopped = "stopped",
             device_class = "shutter" },
    states = {
      state = function()
        local cover_state = feat(object, 0)
        return cover_state == 1 and "opening" or cover_state == 2 and "closing" or "stopped"
      end,
      position = function() return num(feat(object, 7)) end,
    },
    cmds = {
      set = function(payload)
        if payload == "OPEN" then object:execute(5) elseif payload == "CLOSE" then object:execute(6)
        elseif payload == "STOP" then object:execute(3) end
      end,
      ["position/set"] = function(payload)
        local position = tonumber(payload)
        if position then
          object:execute(10, math.max(0, math.min(100, math.floor(position + 0.5))))
        end
      end,
    },
  }
end

-- Inputs/buttons: Grenton events (OnSwitchOn, OnClick, ...) as an HA event entity.
KINDS.event = function(object, options)
  local event_map = options.events or BUTTON_EVENTS
  local device_class = options.device_class -- false = none
  if device_class == nil then device_class = "button" end
  local event_types, seen = {}, {}
  for event_index = 0, 63 do
    local event_type = event_map[event_index]
    if event_type and not seen[event_type] then
      event_types[#event_types + 1] = event_type
      seen[event_type] = true
    end
  end
  return {
    comp = "event",
    suffix = "event",
    topic = "event",
    disc = { state_topic = "~/event", event_types = event_types,
             device_class = device_class or nil },
    events = event_map,
  }
end

-- Default mapping per object class (from the module interfaces).
