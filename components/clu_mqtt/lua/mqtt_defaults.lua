-- @file mqtt_defaults.lua
local DEFAULTS = {
  [3] = { { kind = "binary_sensor" }, { kind = "event" } },                         -- DIN
  [4] = { { kind = "switch" } },                                                    -- DOUT
  [5] = { { kind = "light" } },                                                     -- DIMM
  [6] = { { kind = "sensor", feature = 3, unit = "s", device_class = "duration",
           fn = function(raw_value) return tonumber(raw_value) and tonumber(raw_value) / 1000 end },
         { kind = "button", name = "Start timer", execute = 0 },
         { kind = "button", name = "Stop timer", execute = 1 } },                    -- Timer
  [7] = { { kind = "event" } },                                                     -- BUTTON
  [9] = { { kind = "light" } },                                                     -- LEDRGB
  [12] = { { kind = "sensor", device_class = "voltage", unit = "V" } },            -- PowerSupplyVoltage
  [16] = { { kind = "sensor", feature = 2 },
           { kind = "switch", feature = 1, on = 0, off = 1 } },                     -- Scheduler
  [18] = { { kind = "climate" } },                                                  -- Thermostat v2
  [20] = { { kind = "sensor", device_class = "temperature", unit = "°C" } },        -- PANELSENSTEMP
  [21] = { { kind = "sensor", unit = "%", icon = "mdi:brightness-5" } },            -- PANELSENSLIGHT
  [24] = { { kind = "cover" } },                                                    -- ROLLER_SHUTTER
  [30] = { { kind = "event" } },                                                    -- PANEL_BUTTON
  [40] = { { kind = "binary_sensor", feature = 2, device_class = "occupancy" },
           { kind = "switch", feature = 5, name = "Locked" } },                     -- PresenceSensor v2
  [49] = { { kind = "light" } },                                                    -- LED_CHANNEL
  [72] = { { kind = "light" } },                                                    -- LED_CCT
}

