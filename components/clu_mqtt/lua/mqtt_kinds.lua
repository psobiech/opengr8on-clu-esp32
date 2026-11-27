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

-- opts.fn(value) transforms the raw value (nil = no state).
KINDS.binary_sensor = function(object, options)
  local feature = options.feature or 0
  return {
    comp = "binary_sensor",
    disc = { state_topic = "~/state" },
    states = {
      state = function()
        local raw_value = feat(object, feature)
        if options.fn and raw_value ~= nil then raw_value = options.fn(raw_value) end
        return onoff(raw_value)
      end,
    },
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


-- Number: opts.min / opts.max / opts.step (HA's defaults 1..100 step 1 are too narrow for Grenton
-- values: unbounded 32-bit range, step 0.001 unless given), opts.mode (default "box"), opts.unit.
-- A command sets the feature, clamped to min..max.
KINDS.number = function(object, options)
  local feature = options.feature or 0
  local minimum, maximum = options.min or -2147483648, options.max or 2147483647
  return {
    comp = "number",
    disc = { state_topic = "~/state", command_topic = "~/set", min = minimum, max = maximum,
             step = options.step or 0.001, mode = options.mode or "box",
             unit_of_measurement = options.unit },
    states = { state = function() return num(feat(object, feature)) end },
    cmds = {
      set = function(payload)
        local value = tonumber(payload)
        if not value or value ~= value then return end
        object:set(feature, math.max(minimum, math.min(maximum, value)))
      end,
    },
  }
end

local TEXT_MAX = 255 -- HA's limit for a text entity's state and command

-- Longest prefix of text up to max_length bytes that does not split a UTF-8 sequence.
local function text_prefix(text, max_length)
  if #text <= max_length then return text end
  local cut = max_length
  while cut > 0 and (text:byte(cut + 1) or 0) & 0xC0 == 0x80 do cut = cut - 1 end
  return text:sub(1, cut)
end

-- Text: the feature as a string, cut to opts.max (default TEXT_MAX) bytes; a command sets it
-- verbatim (not upper-cased).
KINDS.text = function(object, options)
  local feature = options.feature or 0
  local max_length = math.min(options.max or TEXT_MAX, TEXT_MAX)
  return {
    comp = "text",
    raw = true,
    disc = { state_topic = "~/state", command_topic = "~/set", max = max_length },
    states = {
      state = function()
        local value = feat(object, feature)
        if value == nil then return nil end
        return text_prefix(tostring(value), max_length)
      end,
    },
    cmds = { set = function(payload) object:set(feature, text_prefix(payload, max_length)) end },
  }
end

-- Select: opts.map = {[value] = "option"} for the feature; choosing an option sets its value.
-- An unmapped value publishes no state.
KINDS.select = function(object, options)
  local feature = options.feature or 0
  local option_names, values_by_option = {}, {}
  for value, option_name in pairs(options.map) do
    option_names[#option_names + 1] = option_name
    values_by_option[option_name] = value
  end
  table.sort(option_names, function(left, right) return values_by_option[left] < values_by_option[right] end)
  return {
    comp = "select",
    raw = true,
    disc = { state_topic = "~/state", command_topic = "~/set", options = option_names },
    states = {
      state = function()
        local value = feat(object, feature)
        return value ~= nil and options.map[math.tointeger(tonumber(value)) or value] or nil
      end,
    },
    cmds = {
      set = function(payload)
        local value = values_by_option[payload]
        if value ~= nil then object:set(feature, value) end
      end,
    },
  }
end

-- Button: payload PRESS calls opts.press(o) or o:execute(opts.execute, table.unpack(opts.args)).
KINDS.button = function(object, options)
  return {
    comp = "button",
    disc = { command_topic = "~/set" },
    cmds = {
      set = function(payload)
        if payload ~= "PRESS" then return end
        if options.press then options.press(object)
        elseif options.execute then object:execute(options.execute, table.unpack(options.args or {})) end
      end,
    },
  }
end

-- TFBUS module (class 2) link state, from the bus master.
KINDS.module = function(object)
  return {
    comp = "binary_sensor",
    link = true, -- the link itself: available while the CLU is, whatever the module's state
    default_name = "TF-Bus link", -- on the module's own device
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
