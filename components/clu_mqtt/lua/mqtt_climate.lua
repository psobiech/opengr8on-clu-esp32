-- @file mqtt_climate.lua
-- Thermostat v2: TargetTemp / CurrentTemp / ControlOutValue (12/14/13); PointValue is writable
-- at feature 3. State (6) selects off/heat; Mode (8) maps to none/away/auto/boost.
KINDS.climate = function(object)
  local presets = { [0] = "none", [1] = "away", [2] = "auto", [3] = "boost" }
  local function temperature(feature_index)
    local temperature_value = tonumber(feat(object, feature_index))
    if temperature_value then return math.floor(temperature_value * 10 + 0.5) / 10 end
  end
  return {
    comp = "climate",
    raw = true,
    disc = {
      current_temperature_topic = "~/state",
      current_temperature_template = "{{ value_json.current_temperature }}",
      temperature_state_topic = "~/state",
      temperature_state_template = "{{ value_json.temperature }}",
      temperature_command_topic = "~/temperature/set",
      mode_state_topic = "~/state",
      mode_state_template = "{{ value_json.mode }}",
      mode_command_topic = "~/mode/set",
      action_topic = "~/state",
      action_template = "{{ value_json.action }}",
      preset_mode_state_topic = "~/state",
      preset_mode_value_template = "{{ value_json.preset_mode }}",
      preset_mode_command_topic = "~/preset/set",
      modes = { "off", "heat" },
      preset_modes = { "none", "away", "auto", "boost" },
      min_temp = 5, max_temp = 35, temp_step = 0.5,
    },
    states = {
      state = function()
        local active = (tonumber(feat(object, 6)) or 0) ~= 0
        local mode = math.tointeger(tonumber(feat(object, 8)) or 0) or 0
        local output = tonumber(feat(object, 13)) or 0
        return json({ temperature = temperature(12), current_temperature = temperature(14),
                      mode = active and "heat" or "off",
                      action = not active and "off" or output > 0 and "heating" or "idle",
                      preset_mode = presets[mode] or "none" })
      end,
    },
    cmds = {
      ["temperature/set"] = function(payload)
        local target_temperature = tonumber(payload)
        if target_temperature then
          target_temperature = math.max(5, math.min(35, target_temperature))
          object:set(3, math.floor(target_temperature * 2 + 0.5) / 2)
        end
      end,
      ["mode/set"] = function(payload)
        payload = payload:lower()
        if payload == "off" then object:execute(1)
        elseif payload == "heat" then object:execute(0) end
      end,
      ["preset/set"] = function(payload)
        payload = payload:lower()
        if payload == "away" then object:execute(5)
        elseif payload == "auto" then object:execute(7)
        elseif payload == "boost" then object:execute(4, 5, 60) -- HeatUp +5 °C for 60 min
        elseif payload == "none" then
          local current_mode = tonumber(feat(object, 8))
          if current_mode == 1 then object:execute(6)
          elseif current_mode == 2 then object:execute(8) end
        end
      end,
    },
  }
end
