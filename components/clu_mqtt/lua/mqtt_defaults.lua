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

-- SMART_PANEL free draw from HA. The panel shows "! TIMEOUT !" unless a draw is flushed right
-- after it, so every entity runs its whole sequence in one call: wake, draw, flush. The draw buffer
-- stays between flushes: Display Clear, then Display Text (once or more) builds a screen. Display X /
-- Y / Font are where and how Display Text prints (kept per panel, not on the panel). Flush:
-- DisplayContent, or Draw on panels with fwApiVer < 5 (docs: TFBUS_SMART_PANEL_FM_4.md).
local PANEL_FONTS = { [1] = "Small", [2] = "Medium", [3] = "Large" }

local function panel_draw_settings(panel)
  panel._mqtt_draw = panel._mqtt_draw or { x = 0, y = 0, font = 1 }
  return panel._mqtt_draw
end

local function panel_flush(panel)
  local module = panel._module
  local module_status = clu.tfbus_module and type(module) == "table" and clu.tfbus_module(module._sn)
  local fw_api_ver = module_status and module_status.fw_api_ver or 0
  panel:execute((fw_api_ver > 0 and fw_api_ver < 5) and 29 or 7)
end

local function panel_setting(setting_name)
  return function(panel) return panel_draw_settings(panel)[setting_name] end,
         function(panel, value) panel_draw_settings(panel)[setting_name] = math.tointeger(value) or math.floor(value) end
end
local read_x, write_x = panel_setting("x")
local read_y, write_y = panel_setting("y")
local read_font, write_font = panel_setting("font")

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
  -- SetBeep (default tone), free draw (see PANEL_FONTS above), ShowButtons; there is no display-off
  -- command. On the TOUCH_PANEL it is only the buzzer.
  [0x03] = { [29] = { { kind = "event", device_class = false,
                        events = { [0] = "gesture_up", [1] = "gesture_down", [2] = "gesture_left",
                                   [3] = "gesture_right", [4] = "proximity", [5] = "page_change",
                                   [6] = "display_on", [7] = "display_off" } },
                      { kind = "button", execute = 0, name_suffix = "Display On", icon = "mdi:monitor" },
                      { kind = "button", execute = 21, name_suffix = "Beep", icon = "mdi:volume-high" },
                      { kind = "button", name_suffix = "Display Clear", icon = "mdi:eraser",
                        press = function(panel)
                          panel:execute(0) -- SwitchOnDisplay
                          panel:execute(2) -- ClearScreen
                          panel_flush(panel)
                        end },
                      { kind = "text", max = 16, name_suffix = "Display Text", icon = "mdi:format-text",
                        read = function(panel) return panel_draw_settings(panel).text or "" end,
                        write = function(panel, text)
                          local settings = panel_draw_settings(panel)
                          settings.text = text
                          panel:execute(0) -- SwitchOnDisplay
                          panel:execute(3, settings.x, settings.y, text, settings.font) -- PrintText
                          panel_flush(panel)
                        end },
                      { kind = "number", name_suffix = "Display X", min = 0, max = 127, step = 1,
                        entity_category = "config", read = read_x, write = write_x },
                      { kind = "number", name_suffix = "Display Y", min = 0, max = 63, step = 1,
                        entity_category = "config", read = read_y, write = write_y },
                      { kind = "select", name_suffix = "Display Font", map = PANEL_FONTS,
                        entity_category = "config", read = read_font, write = write_font },
                      { kind = "button", execute = 1, name_suffix = "Display Show Buttons",
                        icon = "mdi:view-grid" },
                      -- SetPageNr; the state is the page last set from here (pages changed by
                      -- gestures come as page_change events)
                      { kind = "number", name_suffix = "Display Page", min = 1, max = 4, step = 1,
                        icon = "mdi:book-open-page-variant",
                        read = function(panel) return panel_draw_settings(panel).page end,
                        write = function(panel, page)
                          page = math.tointeger(page) or math.floor(page)
                          panel_draw_settings(panel).page = page
                          panel:execute(22, page)
                        end },
                      { kind = "button", execute = 27, name_suffix = "Display Next Page", icon = "mdi:page-next" },
                      { kind = "button", execute = 28, name_suffix = "Display Previous Page",
                        icon = "mdi:page-previous" } } },
}
