-- @file mqtt_light.lua
-- ---------------------------------------------------------------------------------------------
-- Dimmable lights: HA JSON schema (state/command payload {"state":"ON","brightness":0-255,
-- "color":{"r","g","b","w"},"transition":s}). Plain ON / OFF commands are accepted too.
-- ---------------------------------------------------------------------------------------------
local CLS_DIMM, CLS_LEDRGB = 5, 9
local jdecode = __json.decode -- objects.lua

local function b255(value)
  value = math.floor((tonumber(value) or 0) + 0.5)
  return value < 0 and 0 or value > 255 and 255 or value
end

-- Command payload -> table with .state "ON" / "OFF" (nil = not a light command).
local function light_cmd(payload)
  local command = jdecode(payload)
  if type(command) ~= "table" then
    payload = payload:upper()
    command = { state = TRUE[payload] and "ON" or FALSE[payload] and "OFF" or nil }
  end
  if type(command.state) == "string" then command.state = command.state:upper() end
  if command.state ~= "ON" and command.state ~= "OFF" then return nil end
  return command
end

-- "transition" [s] -> ramp [ms] (nil = the object's RampTime).
local function ramp_ms(command)
  local transition_seconds = tonumber(command.transition)
  if transition_seconds and transition_seconds >= 0 then
    return math.floor(transition_seconds * 1000 + 0.5)
  end
end

local function light_disc(mode)
  return { schema = "json", state_topic = "~/state", command_topic = "~/set", brightness = true,
           supported_color_modes = { mode } }
end

-- DIMM: feature 0 Value (0..1) <-> brightness. ON without brightness = SwitchOn with Mode
-- LastValue, OFF = SwitchOff (both take the transition); brightness = set Value (RampTime).
local function dimm_light(object)
  return {
    comp = "light",
    raw = true,
    disc = light_disc("brightness"),
    states = {
      state = function()
        local brightness_value = tonumber(feat(object, 0)) or 0
        if brightness_value <= 0 then return json({ state = "OFF", color_mode = "brightness" }) end
        return json({ state = "ON", color_mode = "brightness",
                      brightness = math.max(1, b255(brightness_value * 255)) })
      end,
    },
    cmds = {
      set = function(payload)
        local command = light_cmd(payload)
        if not command then return end
        local ramp = ramp_ms(command)
        if command.state == "OFF" then
          object:execute(3, 0, ramp)                                -- SwitchOff(Time, Ramp)
        elseif tonumber(command.brightness) then
          object:set(0, b255(command.brightness) / 255)
        else
          object:execute(2, 0, ramp or tonumber(feat(object, 1)), 1) -- SwitchOn(Time, Ramp, Mode)
        end
      end,
    },
  }
end

-- LEDRGB: the output is R/G/B (features 3-5, already scaled by Value) + W (15), 0-255 each.
-- HA sees brightness = the brightest channel and color = the output scaled up to that; commands
-- go out as SetRGBvalue("#rrggbb", Ramp) + SetWhiteValue(white_value, Ramp). ON without a colour restores
-- the last lit colour (SwitchOn before there was one), OFF = SwitchOff.
local function rgbw_light(object)
  local last -- last lit output channels {red, green, blue, white}
  local function output()
    return { b255(feat(object, 3)), b255(feat(object, 4)), b255(feat(object, 5)),
             b255(feat(object, 15)) }
  end
  local function peak(color_channels)
    return math.max(color_channels[1], color_channels[2], color_channels[3], color_channels[4])
  end
  return {
    comp = "light",
    raw = true,
    disc = light_disc("rgbw"),
    states = {
      state = function()
        local output_channels = output()
        local peak_value = peak(output_channels)
        if peak_value == 0 then return json({ state = "OFF", color_mode = "rgbw" }) end
        last = output_channels
        return json({ state = "ON", color_mode = "rgbw", brightness = peak_value,
                      color = { r = b255(output_channels[1] * 255 / peak_value),
                                g = b255(output_channels[2] * 255 / peak_value),
                                b = b255(output_channels[3] * 255 / peak_value),
                                w = b255(output_channels[4] * 255 / peak_value) } })
      end,
    },
    cmds = {
      set = function(payload)
        local command = light_cmd(payload)
        if not command then return end
        local ramp = ramp_ms(command)
        if command.state == "OFF" then object:execute(10, 0, ramp); return end -- SwitchOff(Time, Ramp)
        local output_channels = output()
        local peak_value = peak(output_channels)
        local base = peak_value > 0 and output_channels or last
        local color = type(command.color) == "table" and command.color
        if not color and not tonumber(command.brightness) then
          if peak_value > 0 then return end
          if not base then object:execute(9, 0, ramp); return end  -- SwitchOn(Time, Ramp)
        end
        local base_brightness = base and peak(base) or 0
        local rgbw_channels
        if color then
          rgbw_channels = { b255(color.r), b255(color.g), b255(color.b), b255(color.w) }
        elseif base_brightness > 0 then
          rgbw_channels = { base[1] * 255 / base_brightness, base[2] * 255 / base_brightness,
                            base[3] * 255 / base_brightness, base[4] * 255 / base_brightness }
        else
          rgbw_channels = { 255, 255, 255, 0 }
        end
        local brightness = tonumber(command.brightness) and b255(command.brightness)
                           or (base_brightness > 0 and base_brightness) or 255
        for channel_index = 1, 4 do
          rgbw_channels[channel_index] = b255(rgbw_channels[channel_index] * brightness / 255)
        end
        object:execute(6, string.format("#%02x%02x%02x", rgbw_channels[1], rgbw_channels[2],
                                       rgbw_channels[3]), ramp) -- SetRGBvalue
        object:execute(12, rgbw_channels[4], ramp)              -- SetWhiteValue
      end,
    },
  }
end

-- LED_CHANNEL (one channel of LED_RGBW, e.g. a separate lamp) and LED_CCT (warm + cold channel
-- pair): Value (0-255) = brightness. brightness = SetValue(value, Ramp), ON without brightness =
-- SwitchOn with Mode LastValue, OFF = SwitchOff; all take the transition. LED_CCT also maps
-- ColorTemp (2700-6500 K, HA in kelvin) <-> SetColorTemp(K, Ramp).
local CLS_LEDCH, CLS_LEDCCT = 49, 72
local LEDCH = { value = 0, set = 0, on = 1, off = 2 }               -- feature / method indices
local LEDCCT = { value = 2, set = 2, on = 4, off = 5, ct = 0, set_ct = 0 }
local CT_MIN, CT_MAX = 2700, 6500

local function channel_light(object, indices)
  local mode = indices.ct and "color_temp" or "brightness"
  local disc = light_disc(mode)
  if indices.ct then
    disc.color_temp_kelvin, disc.min_kelvin, disc.max_kelvin = true, CT_MIN, CT_MAX
  end
  return {
    comp = "light",
    raw = true,
    disc = disc,
    states = {
      state = function()
        local brightness = b255(feat(object, indices.value))
        local state = { state = brightness > 0 and "ON" or "OFF", color_mode = mode }
        if brightness > 0 then state.brightness = brightness end
        if indices.ct then state.color_temp = math.tointeger(tonumber(feat(object, indices.ct))) end
        return json(state)
      end,
    },
    cmds = {
      set = function(payload)
        local command = light_cmd(payload)
        if not command then return end
        local ramp = ramp_ms(command)
        if command.state == "OFF" then object:execute(indices.off, 0, ramp); return end -- SwitchOff(Time, Ramp)
        local color_temp = indices.ct and tonumber(command.color_temp)
        if color_temp then
          color_temp = math.floor(math.max(CT_MIN, math.min(CT_MAX, color_temp)) + 0.5)
          object:execute(indices.set_ct, color_temp, ramp)                 -- SetColorTemp(K, Ramp)
        end
        if tonumber(command.brightness) then
          object:execute(indices.set, b255(command.brightness), ramp)    -- SetValue(Value, Ramp)
        elseif not color_temp or b255(feat(object, indices.value)) == 0 then
          object:execute(indices.on, 0, ramp, 1)                         -- SwitchOn(Time, Ramp, Mode)
        end
      end,
    },
  }
end

