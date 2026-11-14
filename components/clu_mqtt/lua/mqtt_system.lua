-- @file mqtt_system.lua
-- ---------------------------------------------------------------------------------------------
-- Internals (cfg.diag_ms set = menuconfig CLU_MQTT_DIAGNOSTICS): local CLU object, TFBUS modules
-- and firmware values from C.sys_info(). All in HA's "Diagnostic" / "Configuration" sections.
-- ---------------------------------------------------------------------------------------------
local DIAG = "diagnostic"
local SYS_NAME = "system" -- MQTT.hide("system") hides the firmware entities

-- Boot time from Uptime: stable timestamp (republished only when it moves > 60 s, i.e. on a
-- reboot or a clock step), unknown until the clock is set.
local boot_epoch
local function boot_time(uptime_seconds)
  local current_epoch = os.time()
  if current_epoch < 1600000000 then return nil end
  local boot_epoch_candidate = current_epoch - (tonumber(uptime_seconds) or 0)
  if not boot_epoch or math.abs(boot_epoch_candidate - boot_epoch) > 60 then
    boot_epoch = boot_epoch_candidate
  end
  return os.date("!%Y-%m-%dT%H:%M:%S+00:00", boot_epoch)
end

local function kib(bytes) return (tonumber(bytes) or 0) // 1024 end

local system_cache, system_cache_time_ms = {}, nil
local SYSOBJ = { _name = SYS_NAME, _class = -1 }
function SYSOBJ.get(_, key)
  local current_time_ms = clu.millis()
  if not system_cache_time_ms or current_time_ms - system_cache_time_ms >= 1000 then
    system_cache = clu.sys_info and clu.sys_info() or {}
    system_cache.lua_mem = math.floor(collectgarbage("count"))
    system_cache_time_ms = current_time_ms
  end
  return system_cache[key]
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
    device_class = "restart", entity_category = "config", press = function() clu.sys_restart() end },
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
  if clu.gate then table.remove(DEFAULTS[0], 2) end -- GATE: feature 2 is PrimaryDNS, there is no State
  DEFAULTS[2] = { { kind = "module", group = "system/tfbus/modules", leaf = true,   -- TFBUS module
                    entity_category = DIAG } }
end

