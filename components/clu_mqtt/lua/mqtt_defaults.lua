-- @file mqtt_defaults.lua
-- TF-Bus module hwType -> model (Home Assistant device per module, "<model> <serial>").
local MODULE_MODELS = {
  [0x03] = "SMART PANEL FM 4",
  [0x15] = "RELAY DIN 4",
  [0x17] = "ROLLER SH DIN",
  [0x18] = "LED RGBW DIN",
  [0x1a] = "DIMMER MOSFET DIN",
  [0x1c] = "TOUCH PANEL FM 4",
  [0x1e] = "IO MODULE DIN 8",
  [0x2a] = "ROLLER SH DIN 3",
}

local DEFAULTS = {
  [3] = { { kind = "binary_sensor" }, { kind = "event" } },                         -- DIN
  [4] = { { kind = "switch" } },                                                    -- DOUT
  [5] = { { kind = "light" } },                                                     -- DIMM
  [6] = { { kind = "sensor", feature = 3, unit = "s", device_class = "duration",
           -- Whole seconds, rounded up (a countdown reads 1 until it ends): the raw ms change on
           -- every scan, unrounded a running timer would publish a state each scan.
           fn = function(raw_value)
             local ms = tonumber(raw_value)
             return ms and math.ceil(ms / 1000)
           end },
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

