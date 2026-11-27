-- @file mqtt_defaults.lua
-- TF-Bus module hwType -> model (Home Assistant device per module, "<model> <serial>").
local MODULE_MODELS = {
  [0x03] = "SMART PANEL FM 4",
  [0x15] = "RELAY DIN 4",
  [0x17] = "ROLLER SH DIN",
  [0x18] = "LED RGBW DIN",
  [0x19] = "ANALOG DIN",
  [0x1a] = "DIMMER MOSFET DIN",
  [0x1c] = "TOUCH PANEL FM 4",
  [0x1e] = "IO MODULE DIN 8",
  [0x2a] = "ROLLER SH DIN 3",
  [0xff] = "ONE WIRE",
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
         { kind = "button", name_suffix = "start", execute = 0 },
         { kind = "button", name_suffix = "stop", execute = 1 } },                   -- Timer
  [7] = { { kind = "event" } },                                                     -- BUTTON
  [9] = { { kind = "light" } },                                                     -- LEDRGB
  [12] = { { kind = "sensor", device_class = "voltage", unit = "V" } },            -- PowerSupplyVoltage
  -- Calendar: State (Start / Stop), ToNextRun [min], OnCalendar
  [15] = { { kind = "switch", feature = 3, on = 0, off = 1 },
           { kind = "sensor", feature = 2, unit = "min", device_class = "duration",
             name_suffix = "next run" },
           { kind = "event", events = { [0] = "calendar" }, device_class = false } },
  [16] = { { kind = "sensor", feature = 2 },
           { kind = "switch", feature = 1, on = 0, off = 1, name_suffix = "running" } }, -- Scheduler
  [18] = { { kind = "climate" } },                                                  -- Thermostat v2
  [20] = { { kind = "sensor", device_class = "temperature", unit = "°C" } },        -- PANELSENSTEMP
  [21] = { { kind = "sensor", unit = "%", icon = "mdi:brightness-5" } },            -- PANELSENSLIGHT
  [23] = { { kind = "sensor", device_class = "temperature", unit = "°C" } },        -- ONEW_SENSOR
  [24] = { { kind = "cover" } },                                                    -- ROLLER_SHUTTER
  -- PANEL_BUTTON (SMART_PANEL_FM_4), each its own device under the panel: events; the LED methods
  -- as buttons, with the LED_CMD byte OM passes (drv_smart_panel_fm4.c io_execute): LedSwitchOn,
  -- RedLedSwitchOn, LedSwitchOff (both colours: there is no per-colour off), ShowOK / ShowError
  -- (green / red flash for 2 s). The panel reports no LED state, so no switch.
  [30] = { { kind = "event", subdevice = true },
           { kind = "button", execute = 1, args = { 2 }, name_suffix = "LED Green On",
             icon = "mdi:led-on", subdevice = true },
           { kind = "button", execute = 2, args = { 16 }, name_suffix = "LED Red On",
             icon = "mdi:led-on", subdevice = true },
           { kind = "button", execute = 2, args = { 4 }, name_suffix = "LED Off",
             icon = "mdi:led-off", subdevice = true },
           { kind = "button", execute = 0, args = { 0 }, name_suffix = "LED Green Blink",
             icon = "mdi:led-outline", subdevice = true },
           { kind = "button", execute = 0, args = { 1 }, name_suffix = "LED Red Blink",
             icon = "mdi:led-outline", subdevice = true },
           { kind = "text", feature = 4, max = 15, name_suffix = "Label", icon = "mdi:label",
             entity_category = "config", subdevice = true },                       -- Label / SetLabel
           { kind = "select", feature = 1, name_suffix = "Mode", entity_category = "config",
             map = { [0] = "Monostable", [1] = "Bistable", [2] = "Locked" }, subdevice = true } }, -- Mode / SetMode
  -- SunriseSunsetCalendar: today's sunrise / sunset (UTC epoch, -1 = none), IsDayNow (-1 = polar
  -- day / night: unknown)
  [39] = { { kind = "sensor", feature = 7, device_class = "timestamp", fn = iso_time,
             name_suffix = "sunrise" },
           { kind = "sensor", feature = 8, device_class = "timestamp", fn = iso_time,
             name_suffix = "sunset" },
           { kind = "binary_sensor", feature = 11, name_suffix = "day",
             fn = function(is_day) if is_day ~= -1 then return is_day end end } },
  [40] = { { kind = "binary_sensor", feature = 2, device_class = "occupancy" },
           { kind = "switch", feature = 5, name_suffix = "locked" } },              -- PresenceSensor v2
  [47] = { { kind = "sensor" } },                                                   -- Statistics
  [48] = { { kind = "sensor", feature = 10, state_class = "total_increasing" } },   -- StatisticsPulse
  [49] = { { kind = "light" } },                                                    -- LED_CHANNEL
  [72] = { { kind = "light" } },                                                    -- LED_CCT
  [73] = { { kind = "sensor", feature = 1, name_suffix = "active sensors",           -- ONE_WIRE_HUB
             icon = "mdi:thermometer-lines", entity_category = "diagnostic" },
           { kind = "button", execute = 0, name_suffix = "refresh sensors",
             entity_category = "config" } },
}

-- Module hwType -> classes mapped differently on that module than in DEFAULTS.
local MODULE_DEFAULTS = {
  -- ANALOG_DIN: class 12 is AnalogIN (Value = RawValue x Scale), not PowerSupplyVoltage: the unit
  -- depends on the input and its Scale.
  [0x19] = { [12] = { { kind = "sensor" } } },
  -- SMART_PANEL_FM_4: PANEL (class 29) events and methods (drv_smart_panel_fm4.c): SwitchOnDisplay,
  -- SetBeep (default tone); there is no display-off command. On the TOUCH_PANEL it is only the buzzer.
  [0x03] = { [29] = { { kind = "event", device_class = false,
                        events = { [0] = "gesture_up", [1] = "gesture_down", [2] = "gesture_left",
                                   [3] = "gesture_right", [4] = "proximity", [5] = "page_change",
                                   [6] = "display_on", [7] = "display_off" } },
                      { kind = "button", execute = 0, name_suffix = "display on", icon = "mdi:monitor" },
                      { kind = "button", execute = 21, name_suffix = "beep", icon = "mdi:volume-high" } } },
}
