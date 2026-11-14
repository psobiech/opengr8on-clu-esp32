-- Virtual CLU objects (declared in OM.LUA as OBJECT:new(class, "NAME")) and the remote CLU proxy
-- (OBJECT:new(1, ip, "NAME")). Loaded after prelude.lua. Interfaces: docs/device-interfaces/object_*.xml.
--
-- Each class: cls.init(object), cls.G[idx](object), cls.S[idx](object, value),
-- cls.X[idx](object, ...), cls.tick(object, now_ms).
-- Features without a getter/setter fall back to the generic store object._f.
local clu = __clu
local VCLASS = __vclass
local lit = __lit
local HOOKS = __hooks

local function vclass(id, def)
  def.G = def.G or {}
  def.S = def.S or {}
  def.X = def.X or {}
  def.__index = def
  def.__name = "OBJECT"
  setmetatable(def, { __index = OBJECT })

  function def:get(idx)
    local getter = def.G[idx]
    if getter then return getter(self) end
    return self._f[idx]
  end

  function def:set(idx, value)
    local setter = def.S[idx]
    if setter then setter(self, value) else self._f[idx] = value end
  end

  function def:execute(idx, ...)
    local executor = def.X[idx]
    if executor then return executor(self, ...) end
    -- execute(idx, value) on a settable feature behaves like set (OM uses both forms)
    if def.S[idx] then return def.S[idx](self, ...) end
    clu.log(2, self._name .. ":execute(" .. tostring(idx) .. ") not implemented")
  end

  VCLASS[id] = def
  return def
end

local function now_s() return os.time() end

-- ---------------------------------------------------------------------------------------------
-- JSON, shared by the extensions as __json (MQTT discovery / object API, HttpRequest / HttpListener).
-- encode: strings, numbers, booleans, arrays, objects (keys as strings); anything else is null.
-- ---------------------------------------------------------------------------------------------
local JESC = { ['"'] = '\\"', ["\\"] = "\\\\", ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t" }

local function json(value)
  local value_type = type(value)
  if value_type == "string" then
    return '"' .. value:gsub('[%c"\\]', function(character) return JESC[character] or string.format("\\u%04x", character:byte()) end) .. '"'
  elseif value_type == "number" then
    if math.type(value) == "integer" then return string.format("%d", value) end
    if value ~= value or value == math.huge or value == -math.huge then return "null" end
    return string.format("%.10g", value)
  elseif value_type == "boolean" then
    return value and "true" or "false"
  elseif value_type == "table" then
    local parts = {}
    if #value > 0 then
      for _, x in ipairs(value) do parts[#parts + 1] = json(x) end
      return "[" .. table.concat(parts, ",") .. "]"
    end
    local keys = {}
    for k in pairs(value) do keys[#keys + 1] = k end
    table.sort(keys)
    for _, k in ipairs(keys) do parts[#parts + 1] = json(tostring(k)) .. ":" .. json(value[k]) end
    return "{" .. table.concat(parts, ",") .. "}"
  end
  return "null"
end

local JUNESC = { b = "\b", f = "\f", n = "\n", r = "\r", t = "\t", ['"'] = '"', ["\\"] = "\\", ["/"] = "/" }

-- JSON text -> value, or nil, err. null decodes to nil (in arrays: holes, n kept in .n).
-- Nesting deeper than max_depth (default 8) is an error.
local function jdecode(json_text, max_depth)
  max_depth = max_depth or 8
  local pos = 1
  local function ws() pos = json_text:find("[^ \t\r\n]", pos) or #json_text + 1 end
  local parse_value
  local function str()
    local out = {}
    pos = pos + 1
    while true do
      local character = json_text:sub(pos, pos)
      if character == "" then error("unterminated string") end
      if character == '"' then pos = pos + 1; return table.concat(out) end
      if character == "\\" then
        local escape_code = json_text:sub(pos + 1, pos + 1)
        if escape_code == "u" then
          local hex_digits = json_text:sub(pos + 2, pos + 5)
          if not hex_digits:match("^%x%x%x%x$") then error("bad \\u escape") end
          out[#out + 1] = utf8.char(tonumber(hex_digits, 16))
          pos = pos + 6
        elseif JUNESC[escape_code] then
          out[#out + 1] = JUNESC[escape_code]
          pos = pos + 2
        else
          error("bad escape")
        end
      elseif character:byte() < 32 then
        error("control character in string")
      else
        local run = json_text:match('^[^"\\%c]+', pos)
        out[#out + 1] = run
        pos = pos + #run
      end
    end
  end
  function parse_value(depth)
    if depth > max_depth then error("too deep") end
    ws()
    local next_character = json_text:sub(pos, pos)
    if next_character == '"' then return str() end
    if next_character == "[" then
      local array, array_length = {}, 0
      pos = pos + 1
      ws()
      if json_text:sub(pos, pos) == "]" then pos = pos + 1; array.n = 0; return array end
      while true do
        array_length = array_length + 1
        array[array_length] = parse_value(depth + 1)
        ws()
        next_character = json_text:sub(pos, pos)
        pos = pos + 1
        if next_character == "]" then array.n = array_length; return array end
        if next_character ~= "," then error("expected , or ]") end
      end
    end
    if next_character == "{" then
      local object = {}
      pos = pos + 1
      ws()
      if json_text:sub(pos, pos) == "}" then pos = pos + 1; return object end
      while true do
        ws()
        if json_text:sub(pos, pos) ~= '"' then error("expected key") end
        local key = str()
        ws()
        if json_text:sub(pos, pos) ~= ":" then error("expected :") end
        pos = pos + 1
        object[key] = parse_value(depth + 1)
        ws()
        next_character = json_text:sub(pos, pos)
        pos = pos + 1
        if next_character == "}" then return object end
        if next_character ~= "," then error("expected , or }") end
      end
    end
    if json_text:sub(pos, pos + 3) == "true" then pos = pos + 4; return true end
    if json_text:sub(pos, pos + 4) == "false" then pos = pos + 5; return false end
    if json_text:sub(pos, pos + 3) == "null" then pos = pos + 4; return nil end
    local number_text = json_text:match("^-?%d+%.?%d*[eE]?[-+]?%d*", pos)
    local parsed_number = number_text and tonumber(number_text)
    if not parsed_number then error("unexpected input at " .. pos) end
    pos = pos + #number_text
    return parsed_number
  end
  local ok, decoded_value = pcall(function()
    local result = parse_value(0)
    ws()
    if pos <= #json_text then error("trailing data") end
    return result
  end)
  if not ok then return nil, tostring(decoded_value):gsub("^.-:%d+: ", "") end
  return decoded_value
end
__json = { encode = json, decode = jdecode }
__defclass = vclass -- extensions (http.lua) define their classes with it


-- ---------------------------------------------------------------------------------------------
-- Persistent store: one file on the FAT data partition (wear-levelled, 6 MB), default
-- a:\STORAGE.DAT. Content is a Lua table literal {section = {key = value}}. Writes are
-- debounced (PERSIST_DELAY_MS) to save flash; flush(true) writes now. Save = write .TMP, remove
-- .DAT, rename; load falls back to .TMP if a power cut hit between remove and rename.
-- ---------------------------------------------------------------------------------------------
local PERSIST_DELAY_MS = 5000
local persist = { data = nil, dirty = false, due = nil }
local persist_path = clu.storage or ((clu.root or ".") .. "/STORAGE.DAT")
local persist_tmp = persist_path:gsub("%.%w+$", "") .. ".TMP"

-- Serialize nil/boolean/number/string and tables of them (no functions/userdata/cycles).
local function ser(value, depth)
  local value_type = type(value)
  if value_type == "nil" or value_type == "boolean" then return tostring(value)
  elseif value_type == "number" then
    if math.type(value) == "integer" then return string.format("%d", value) end
    if value ~= value then return "0/0" elseif value == math.huge then return "1/0" elseif value == -math.huge then return "-1/0" end
    local formatted_number = string.format("%.17g", value)
    return formatted_number:find("[%.eEn]") and formatted_number or formatted_number .. ".0"
  elseif value_type == "string" then return string.format("%q", value)
  elseif value_type == "table" then
    depth = (depth or 0) + 1
    if depth > 16 then return nil end
    local parts = {}
    for k, x in pairs(value) do
      local sk, sx = ser(k, depth), ser(x, depth)
      if sk and sx then parts[#parts + 1] = "[" .. sk .. "]=" .. sx end
    end
    table.sort(parts)
    return "{" .. table.concat(parts, ",") .. "}"
  end
  return nil
end
__ser = ser

local function read_file(path)
  local file = io.open(path, "rb")
  if not file then return nil end
  local txt = file:read("a")
  file:close()
  return txt
end

function persist.load()
  if persist.data then return persist.data end
  persist.data = {}
  for _, path in ipairs({ persist_path, persist_tmp }) do
    local txt = read_file(path)
    if txt then
      local fn = load("return " .. txt, "=storage", "t", {})
      local ok, data = pcall(fn or error)
      if ok and type(data) == "table" then
        persist.data = data
        break
      end
      clu.log(1, "storage: " .. path .. " unreadable, ignored")
    end
  end
  return persist.data
end

function persist.section(name)
  local data = persist.load()
  data[name] = data[name] or {}
  return data[name]
end

function persist.mark(now)
  persist.dirty = true
  persist.due = persist.due or ((now or clu.millis()) + PERSIST_DELAY_MS)
end

function persist.size() return #(ser(persist.load()) or "") end

function persist.flush(force, now)
  if not persist.dirty then return true end
  if not force and (now or clu.millis()) < persist.due then return true end
  local txt = ser(persist.load()) or "{}"
  local file = io.open(persist_tmp, "wb")
  local ok = file and file:write(txt) and file:close()
  if ok then
    os.remove(persist_path)
    ok = os.rename(persist_tmp, persist_path)
  end
  if not ok then
    clu.log(1, "storage: write " .. persist_path .. " failed")
    persist.due = (now or clu.millis()) + PERSIST_DELAY_MS -- retry later
    return false
  end
  persist.dirty, persist.due = false, nil
  return true
end

HOOKS.loop[#HOOKS.loop + 1] = function(now) persist.flush(false, now) end
-- Reset / reboot within PERSIST_DELAY_MS of a change: write it now (stays the last close hook).
HOOKS.close[#HOOKS.close + 1] = function() persist.flush(true) end
__persist = persist

-- ---------------------------------------------------------------------------------------------
-- cron: "min hour dom mon dow" with *, a-b, */n, a-b/n, lists. dow 0..7 (0 and 7 = Sunday).
-- ---------------------------------------------------------------------------------------------
local cron = {}

local function cron_field(field_text, lo, hi)
  local set, any = {}, field_text == "*"
  for part in field_text:gmatch("[^,]+") do
    local range, step = part:match("^([^/]+)/(%d+)$")
    range = range or part
    step = tonumber(step) or 1
    if step < 1 then return nil end
    local range_start, range_end
    if range == "*" then
      range_start, range_end = lo, hi
    else
      range_start, range_end = range:match("^(%d+)-(%d+)$")
      if range_start then
        range_start, range_end = tonumber(range_start), tonumber(range_end)
      else
        range_start = tonumber(range:match("^(%d+)$"))
        if not range_start then return nil end
        range_end = (part:find("/")) and hi or range_start
      end
    end
    if range_start < lo or range_end > hi or range_start > range_end then return nil end
    for value = range_start, range_end, step do set[value] = true end
  end
  return set, any
end

function cron.parse(rule)
  if type(rule) ~= "string" then return nil end
  local fields = {}
  for w in rule:gmatch("%S+") do fields[#fields + 1] = w end
  if #fields ~= 5 then return nil end
  local parsed_rule = {}
  local any
  parsed_rule.min = cron_field(fields[1], 0, 59)
  parsed_rule.hour = cron_field(fields[2], 0, 23)
  parsed_rule.dom, parsed_rule.dom_any = cron_field(fields[3], 1, 31)
  parsed_rule.mon = cron_field(fields[4], 1, 12)
  parsed_rule.dow, any = cron_field(fields[5], 0, 7)
  parsed_rule.dow_any = any
  if not (parsed_rule.min and parsed_rule.hour and parsed_rule.dom and parsed_rule.mon and parsed_rule.dow) then return nil end
  if parsed_rule.dow[7] then parsed_rule.dow[0] = true end
  return parsed_rule
end

local function day_match(rule, date)
  local dom, dow = rule.dom[date.day], rule.dow[date.wday - 1]
  if rule.dom_any and rule.dow_any then return true end
  if rule.dom_any then return dow end
  if rule.dow_any then return dom end
  return dom or dow -- classic cron: either restricted field matches
end

function cron.match(rule, ts)
  local date = os.date("*t", ts)
  return rule.min[date.min] and rule.hour[date.hour] and rule.mon[date.month] and day_match(rule, date) or false
end

-- First matching minute strictly after ts (nil if none within ~4 years, e.g. "0 0 31 2 *").
function cron.next(rule, ts)
  local date = os.date("*t", ts - ts % 60 + 60)
  date.sec = 0
  for _ = 1, 4 * 366 * 24 + 60 * 24 do
    if not rule.mon[date.month] then
      date.month, date.day, date.hour, date.min = date.month + 1, 1, 0, 0
    elseif not day_match(rule, date) then
      date.day, date.hour, date.min = date.day + 1, 0, 0
    elseif not rule.hour[date.hour] then
      date.hour, date.min = date.hour + 1, 0
    elseif not rule.min[date.min] then
      date.min = date.min + 1
      if date.min > 59 then date.hour, date.min = date.hour + 1, 0 end
    else
      return os.time(date)
    end
    date = os.date("*t", os.time(date)) -- normalise (fills wday, rolls over)
  end
  return nil
end

__cron = cron

-- cron.next cached per wall-clock minute (features polled by clientReport every loop).
local function next_cached(object, key, fn)
  local minute = os.time() // 60
  local cache = object._nc
  if not cache or cache.m ~= minute then cache = { m = minute }; object._nc = cache end
  if cache[key] == nil then cache[key] = { fn() } end
  return cache[key][1]
end

-- Minute-boundary helper for cron-driven objects: true once per wall-clock minute.
local function new_minute(object)
  local minute = now_s() // 60
  if object._minute == minute then return false end
  local first = object._minute == nil
  object._minute = minute
  return not first
end

-- ---------------------------------------------------------------------------------------------
-- Timer (class 6): Time[ms], Mode 0 countdown / 1 interval, State 0 stopped / 1 counting / 2 paused,
-- Value = ms left. Events: 0 OnTimer, 1 OnStart, 2 OnStop, 3 OnPause.
-- ---------------------------------------------------------------------------------------------
local Timer = vclass(6, {})

function Timer.init(object)
  object._f[0], object._f[1] = 1000, 0
  object._state, object._left = 0, 1000
end

Timer.G[2] = function(object) return object._state end
Timer.G[3] = function(object)
  if object._state == 0 then return 0 end
  return math.max(0, math.floor(object._left))
end
Timer.S[0] = function(object, value)
  object._f[0] = tonumber(value) or object._f[0]
  if object._state == 1 then object._left, object._last = object._f[0], clu.millis() end
end
Timer.S[1] = function(object, value) object._f[1] = (tonumber(value) or 0) ~= 0 and 1 or 0 end

Timer.X[0] = function(object) -- Start
  object._left, object._last, object._state = tonumber(object._f[0]) or 1000, clu.millis(), 1
  object:_fire(1)
end
Timer.X[1] = function(object) -- Stop
  object._state, object._left = 0, tonumber(object._f[0]) or 1000
  object:_fire(2)
end
Timer.X[2] = function(object) -- Pause: pauses a running timer, resumes a paused one
  if object._state == 1 then
    object._state = 2
    object:_fire(3)
  elseif object._state == 2 then
    object._state, object._last = 1, clu.millis()
  end
end

function Timer.tick(object, now)
  if object._state ~= 1 then return end
  object._left = object._left - (now - object._last)
  object._last = now
  if object._left > 0 then return end
  local period = tonumber(object._f[0]) or 1000
  if object._f[1] == 1 and period > 0 then
    object._left = object._left % period
    if object._left == 0 then object._left = period end
  else
    -- Countdown end: the timer stops (OnStop), then OnTimer, like the vCLU (Timer.ts).
    -- UNVERIFIED on a real CLU.
    object._state, object._left = 0, period
    object:_fire(2)
  end
  object:_fire(0)
end

-- ---------------------------------------------------------------------------------------------
-- Calendar (class 15): Rule (cron), SinceLastRun / ToNextRun [min], State.
-- Events: 0 OnCalendar, 1 OnStart, 2 OnStop, 3 OnCancel.
-- ---------------------------------------------------------------------------------------------
local Calendar = vclass(15, {})

function Calendar.init(object)
  object._f[0], object._f[3] = "", 1
  object._cancel = 0
end

Calendar.S[0] = function(object, value)
  object._nc = nil
  object._rule = cron.parse(value)
  object._f[0] = object._rule and value or "ERROR"
end
Calendar.S[3] = function() end -- read-only
Calendar.G[1] = function(object)
  if not object._lastrun then return 0 end
  return (now_s() - object._lastrun) // 60
end
Calendar.G[2] = function(object)
  if not object._rule or object._f[3] ~= 1 then return 0 end
  local next_time = next_cached(object, "n", function() return cron.next(object._rule, now_s()) end)
  return next_time and (next_time - now_s() + 59) // 60 or 0
end
Calendar.X[0] = function(object) object._f[3] = 1; object:_fire(1) end
Calendar.X[1] = function(object) object._f[3] = 0; object:_fire(2) end
Calendar.X[3] = function(object, cancel_count)
  object._cancel = object._cancel + (tonumber(cancel_count) or 1)
  object:_fire(3)
end

function Calendar.tick(object)
  if not new_minute(object) or not object._rule or object._f[3] ~= 1 then return end
  local current_time = now_s()
  if not cron.match(object._rule, current_time) then return end
  if object._cancel > 0 then
    object._cancel = object._cancel - 1
    return
  end
  object._lastrun = current_time
  object:_fire(0)
end

-- ---------------------------------------------------------------------------------------------
-- EventScheduler (class 43): up to 16 cron rules {id, state, "cron"}.
-- Events: 0 OnStart, 1 OnStop, 2 OnEvent, 3 OnRuleAdd, 4 OnRuleDelete.
-- Rules persist across reboots (persist section "es", key = object name). AddRule of a rule
-- already present returns its id without adding a duplicate (OnInit code re-adding rules each
-- boot stays idempotent) — real CLU behaviour not known.
-- ---------------------------------------------------------------------------------------------
local ES = vclass(43, {})
local ES_MAX = 16

local function es_fmt(rule) return "{" .. rule.id .. "," .. rule.state .. "," .. lit(rule.cron) .. "}" end

local function es_sorted(object)
  local sorted_rules = {}
  for _, r in pairs(object._rules) do sorted_rules[#sorted_rules + 1] = r end
  table.sort(sorted_rules, function(left, right) return left.id < right.id end)
  return sorted_rules
end

local function es_next(object)
  local best_rule, best_time = nil, nil
  for _, rule in ipairs(es_sorted(object)) do
    if rule.state == 1 then
      local next_time = cron.next(rule.parsed, now_s())
      if next_time and (not best_time or next_time < best_time) then best_rule, best_time = rule, next_time end
    end
  end
  return best_rule
end

local function es_save(object)
  local saved_rules = {}
  for _, rule in ipairs(es_sorted(object)) do saved_rules[#saved_rules + 1] = { rule.id, rule.state, rule.cron } end
  persist.section("es")[object._name] = saved_rules
  persist.mark()
end

function ES.init(object)
  object._f[5] = 1
  object._rules = {}
  for _, r in ipairs(persist.section("es")[object._name] or {}) do
    local id, state, rule = tonumber(r[1]), tonumber(r[2]), r[3]
    local parsed = type(rule) == "string" and cron.parse(rule)
    if id and parsed and id >= 1 and id <= ES_MAX then
      object._rules[id] = { id = id, state = state == 0 and 0 or 1, cron = rule, parsed = parsed }
    end
  end
end

ES.G[0] = function(object)
  local formatted_rules = {}
  for _, rule in ipairs(es_sorted(object)) do formatted_rules[#formatted_rules + 1] = es_fmt(rule) end
  return "{" .. table.concat(formatted_rules, ",") .. "}"
end
ES.G[1] = function(object)
  if object._current and now_s() - object._current_t < 60 then return es_fmt(object._current) end
  return "N\\A"
end
ES.G[2] = function(object)
  local next_rule = next_cached(object, "n", function() return es_next(object) end)
  return next_rule and es_fmt(next_rule) or "N\\A"
end
ES.G[3] = function(object) return #es_sorted(object) end
ES.G[4] = function(object) return ES_MAX - #es_sorted(object) end
ES.S[5] = function(object, value)
  local normalized_state = (tonumber(value) or 0) ~= 0 and 1 or 0
  if normalized_state == object._f[5] then return end
  object._f[5] = normalized_state
  object:_fire(normalized_state == 1 and 0 or 1)
end

ES.X[0] = function(object) ES.S[5](object, 1) end
ES.X[1] = function(object) ES.S[5](object, 0) end
ES.X[2] = function(object, rule) -- AddRule -> id, 0 on error
  local parsed = cron.parse(rule)
  if not parsed then return 0 end
  for _, r in pairs(object._rules) do
    if r.cron == rule then return r.id end
  end
  for id = 1, ES_MAX do
    if not object._rules[id] then
      object._rules[id] = { id = id, state = 1, cron = rule, parsed = parsed }
      object._nc = nil
      es_save(object)
      object:_fire(3)
      return id
    end
  end
  return 0
end
ES.X[3] = function(object, id) -- DeleteRule -> 0 ok, 1 error
  id = tonumber(id)
  if not id or not object._rules[id] then return 1 end
  object._rules[id] = nil
  object._nc = nil
  es_save(object)
  object:_fire(4)
  return 0
end
ES.X[4] = function(object, id)
  local rule = object._rules[tonumber(id) or -1]
  return rule and es_fmt(rule) or "N\\A"
end
local function es_state(object, id, state)
  local rule = object._rules[tonumber(id) or -1]
  if not rule then return 1 end
  if rule.state ~= state then
    rule.state = state
    es_save(object)
  end
  object._nc = nil
  return 0
end
ES.X[5] = function(object, id) return es_state(object, id, 1) end
ES.X[6] = function(object, id) return es_state(object, id, 0) end
ES.X[7] = function(object) return ES.G[0](object) end
ES.X[8] = function(object) return ES.G[2](object) end
ES.X[9] = function(object)
  local had = next(object._rules) ~= nil
  object._rules = {}
  object._nc = nil
  if had then
    es_save(object)
    object:_fire(4)
  end
  return 0
end

function ES.tick(object)
  if not new_minute(object) or object._f[5] ~= 1 then return end
  local current_time = now_s()
  for _, rule in ipairs(es_sorted(object)) do
    if rule.state == 1 and cron.match(rule.parsed, current_time) then
      object._current, object._current_t = rule, current_time
      object:_fire(2)
    end
  end
end

-- ---------------------------------------------------------------------------------------------
-- PresenceSensor v2 (class 40). Times in seconds.
-- Features: 0 Timeout, 1 State, 2 PresenceDetected, 3 TimeFromLastPresence, 4 DetectionDelay,
--           5 Locked, 6 Mode (0 impulse, 1 state).
-- Events: 0 OnStart, 1 OnStop, 2 OnSwitchOn, 3 OnSwitchOff.
-- ---------------------------------------------------------------------------------------------
local PS = vclass(40, {})

function PS.init(object)
  object._f[0], object._f[1], object._f[2], object._f[4], object._f[5], object._f[6] = 60, 1, 0, 2, 0, 0
  object._last = clu.millis()
  object._ignore_until = 0
  object._holding = false
end

local function ps_on(object)
  if object._f[2] == 0 then
    object._f[2] = 1
    object:_fire(2)
  end
end

local function ps_off(object)
  if object._f[2] == 1 then
    object._f[2] = 0
    object:_fire(3)
  end
end

local function ps_lock(object, value)
  value = (tonumber(value) or 0) ~= 0 and 1 or 0
  if value == 1 then
    object._last = clu.millis()
    if object._f[5] == 0 then
      object._f[5] = 1
      ps_on(object)
    end
  elseif object._f[5] == 1 then
    object._f[5] = 0
    object._ignore_until = clu.millis() + (tonumber(object._f[4]) or 0) * 1000
    ps_off(object)
  end
end

PS.G[3] = function(object)
  if object._holding then return 0 end
  return (clu.millis() - object._last) // 1000
end
PS.S[1] = function(object, value)
  local normalized_state = (tonumber(value) or 0) ~= 0 and 1 or 0
  if normalized_state == object._f[1] then return end
  object._f[1] = normalized_state
  object:_fire(normalized_state == 1 and 0 or 1)
end
PS.S[2] = function() end -- read-only
PS.S[5] = ps_lock
PS.X[0] = function(object) PS.S[1](object, 1) end
PS.X[1] = function(object) PS.S[1](object, 0) end
PS.X[2] = function(object) -- DetectPresence
  local now = clu.millis()
  object._last = now
  if object._f[6] == 1 then object._holding = true end
  if object._f[1] ~= 1 or object._f[5] == 1 or now < object._ignore_until then return end
  ps_on(object)
end
PS.X[3] = function(object) -- UndetectPresence
  if object._holding then
    object._holding = false
    object._last = clu.millis()
  end
end
PS.X[4] = function(object) ps_lock(object, object._f[5] == 1 and 0 or 1) end

function PS.tick(object, now)
  if object._f[2] ~= 1 or object._f[5] == 1 or object._holding or object._f[1] ~= 1 then return end
  if now - object._last >= (tonumber(object._f[0]) or 60) * 1000 then ps_off(object) end
end

-- ---------------------------------------------------------------------------------------------
-- Statistics v1 (class 47) / StatisticsPulse v1 (class 48): CLU interface 0x203 (firmware 5.15).
-- Values are kept locally only: sending measurements to the Grenton cloud is not supported
-- (LastMeasurementSendTime stays ""). StatisticsPulse Value counting pulses is a guess.
-- Statistics: features 0 Value, 1 StatisticState, 2 Load, 4-9 statistic types; event 0 OnValueChange.
-- StatisticsPulse: features 10 Value, 1 StatisticState, 2 Load, 3 StatisticTypePulse;
--   execute 0 TriggerPulse; event 1 OnPulseTriggered.
-- ---------------------------------------------------------------------------------------------
local Stat = vclass(47, {})

function Stat.init(object)
  object._f[0], object._f[1], object._f[2], object._f[6] = 0, 0, 0, 0
  for _, i in ipairs({ 4, 5, 7, 8, 9 }) do object._f[i] = -1 end
end

Stat.S[0] = function(object, value)
  value = tonumber(value) or 0
  if value ~= object._f[0] then
    object._f[0] = value
    object:_fire(0)
  end
end

local Pulse = vclass(48, {})

function Pulse.init(object)
  object._f[10], object._f[1], object._f[2], object._f[3] = 0, 0, 0, -1
end

Pulse.S[10] = function(object, value) object._f[10] = tonumber(value) or 0 end

Pulse.X[0] = function(object) -- TriggerPulse
  object._f[10] = object._f[10] + 1
  object:_fire(1)
end

-- ---------------------------------------------------------------------------------------------
-- Storage (class 44): persistent user variables (globals). Same model as the Java/Node vCLU:
--   STO:execute(4, "name")  Store: track global "name" and save it now; no name = save all tracked
--   STO:execute(3)          EraseAll: forget all variables and wipe them from flash
--   STO:get(1)              StorageUtilization, % of STORAGE_BUDGET used by the store file
-- Tracked variables are restored before OnInit (via OM's setVar when present, so _type applies),
-- then watched every second; changes are written debounced. How the real CLU picks persistent
-- variables is not known (no sample OM.LUA uses them).
-- ---------------------------------------------------------------------------------------------
local Storage = vclass(44, {})
local STORAGE_BUDGET = 64 * 1024
local storage_hooked = false
local storage_next_scan = 0

local function sto_vars() return persist.section("vars") end

local function sto_assign(name, value)
  if type(setVar) == "function" then setVar(name, value) else _G[name] = value end
end

-- Tables are stored as deep copies (round-trip through ser, i.e. exactly what gets saved): an
-- alias would follow in-place edits of the global and hide them from the change check.
local function sto_copy(value, serialized_value)
  if type(value) ~= "table" then return value end
  return load("return " .. (serialized_value or ser(value)), "=storage", "t", {})()
end

local function sto_capture(name)
  local vars = sto_vars()
  local cur = _G[name]
  local serialized_value = ser(cur)
  if serialized_value == nil then
    clu.log(2, "storage: " .. name .. " has unsupported type " .. type(cur))
    return false
  end
  if vars[name] == nil or serialized_value ~= ser(vars[name]) then
    vars[name] = sto_copy(cur, serialized_value)
    persist.mark()
    return true
  end
  return false
end

local function sto_scan(now) -- now nil: scan unconditionally (close hook)
  if now then
    if now < storage_next_scan then return end
    storage_next_scan = now + 1000
  end
  for name in pairs(sto_vars()) do
    if _G[name] ~= nil then sto_capture(name) end
  end
end

function Storage.init(object)
  if storage_hooked then return end
  storage_hooked = true
  HOOKS.init[#HOOKS.init + 1] = function()
    for name, value in pairs(sto_vars()) do sto_assign(name, sto_copy(value)) end
  end
  HOOKS.loop[#HOOKS.loop + 1] = sto_scan
  table.insert(HOOKS.close, 1, sto_scan) -- capture last changes before the final flush
end

Storage.G[1] = function()
  return math.min(100, math.floor(persist.size() * 100 / STORAGE_BUDGET))
end

Storage.X[3] = function() -- EraseAll
  persist.load().vars = {}
  persist.mark()
  persist.flush(true)
end

Storage.X[4] = function(object, name) -- Store
  if type(name) == "string" and name ~= "" then
    sto_capture(name)
  else
    for n in pairs(sto_vars()) do sto_capture(n) end
  end
  if persist.size() > STORAGE_BUDGET then clu.log(2, "storage: over budget") end
  persist.flush(true)
end

-- ---------------------------------------------------------------------------------------------
-- Push (class 31): no Grenton cloud here; Send logs the notification and fires OnSend.
-- Features: 0 Message, 1 Title, 2 LastSendTime, 3 Interval [s]. Events: 0 OnSend, 1 OnOverflow.
-- ---------------------------------------------------------------------------------------------
local Push = vclass(31, {})
local PUSH_MAX = 500

function Push.init(object)
  object._f[0], object._f[1], object._f[2], object._f[3] = "", "", "", 1
end

local function push_append(object, idx, value)
  local message = object._f[idx] .. tostring(value or "")
  if #message > PUSH_MAX then
    message = message:sub(1, PUSH_MAX)
    object:_fire(1)
  end
  object._f[idx] = message
end

Push.S[0] = function(object, value) push_append(object, 0, value) end
Push.S[1] = function(object, value) push_append(object, 1, value) end
Push.S[2] = function() end
Push.X[1] = function(object) object._f[0] = "" end
Push.X[3] = function(object) object._f[1] = "" end
Push.X[4] = function(object)
  local now = clu.millis()
  if object._sent and now - object._sent < (tonumber(object._f[3]) or 1) * 1000 then return end
  object._sent = now
  object._f[2] = os.date("%Y-%m-%d %H:%M:%S")
  clu.log(3, "PUSH " .. object._name .. ": [" .. object._f[1] .. "] " .. object._f[0])
  object:_fire(0)
end

-- ---------------------------------------------------------------------------------------------
-- Weekly schedule "Data" (Scheduler, Thermostat AutoMode), as written by OM 1.12.1:
-- 7 days (index 0 = Sunday) x 96 quarter-hours x 2 chars, each pair a
-- base-90 number 0..8099 (alphabet below), then a 6-char suffix (sign, 2 chars, ':' or '*',
-- 2 chars) that only stores the editor's vertical zoom. Slot value -> Min + raw*(Max-Min)/8100
-- (as OM's chart editor shows it). OM's default slot 5399 = 20.0 with 0..30.
-- ---------------------------------------------------------------------------------------------
local SCHED_ALPHA = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ !#$()*+,-./:;<=>?@[]^_`{|}~"
local SCHED_DIGIT = {}
for i = 1, #SCHED_ALPHA do SCHED_DIGIT[SCHED_ALPHA:byte(i)] = i - 1 end
local SCHED_SCALE = 8100

local sched = {}

-- -> rows[0..6][0..95] = raw (0..8099), or nil if the string is not a schedule.
function sched.decode(data)
  if type(data) ~= "string" or #data < 7 * 96 * 2 then return nil end
  local rows = {}
  for d = 0, 6 do
    local row = {}
    for q = 0, 95 do
      local i = (d * 96 + q) * 2 + 1
      local hi, lo = SCHED_DIGIT[data:byte(i)], SCHED_DIGIT[data:byte(i + 1)]
      if not hi or not lo then return nil end
      row[q] = hi * 90 + lo
    end
    rows[d] = row
  end
  return rows
end

function sched.encode(rows) -- tests / tools; default zoom suffix as written by OM
  local out = {}
  for d = 0, 6 do
    for q = 0, 95 do
      local value = math.min(rows[d][q], SCHED_SCALE - 1)
      local hi, lo = value // 90 + 1, value % 90 + 1
      out[#out + 1] = SCHED_ALPHA:sub(hi, hi) .. SCHED_ALPHA:sub(lo, lo)
    end
  end
  return table.concat(out) .. "+00:zz"
end

-- Raw slot for wall-clock time ts (local time), plus the slot's start (seconds).
function sched.slot(rows, ts)
  local date = os.date("*t", ts)
  local quarter_hour = date.hour * 4 + date.min // 15
  return rows[date.wday - 1][quarter_hour], (date.wday - 1) * 96 + quarter_hour
end

function sched.value(raw, min, max)
  min, max = tonumber(min) or 0, tonumber(max) or 1
  return min + raw * (max - min) / SCHED_SCALE
end
__sched = sched

local function round_to(value, decimals)
  local multiplier = 10 ^ decimals
  local rounded_value = math.floor(value * multiplier + 0.5) / multiplier
  if decimals == 0 then return math.tointeger(rounded_value) or rounded_value end
  return rounded_value
end

-- ---------------------------------------------------------------------------------------------
-- Scheduler (class 16): Features 0 Data, 1 State, 2 Value, 3 Min, 4 Max.
-- Events: 0 OnHarmonogram (every quarter-hour while active, and on Start), 1 OnStart, 2 OnStop.
-- Value rounded like OM's editor shows it: Max <= 1 -> 2 decimals, <= 10 -> 1, else integer.
-- [~] firing on Start and on every slot (not only on change) is a guess from the hint text.
-- ---------------------------------------------------------------------------------------------
local Sched = vclass(16, {})

function Sched.init(object)
  object._f[0], object._f[1], object._f[2], object._f[3], object._f[4] = "", 1, 0, 0, 1
end
Sched.S[0] = function(object, value)
  object._f[0] = tostring(value or "")
  object._rows = sched.decode(object._f[0])
  if object._f[0] ~= "" and not object._rows then clu.log(2, object._name .. ": bad schedule Data") end
  object._slot = nil -- re-evaluate on the next tick
end
Sched.S[1] = function() end
Sched.S[2] = function() end

local function sched_apply(object, slot_id, raw)
  local max = tonumber(object._f[4]) or 1
  local dec = max <= 1 and 2 or max <= 10 and 1 or 0
  object._slot = slot_id
  object._f[2] = round_to(sched.value(raw, object._f[3], max), dec)
  object:_fire(0)
end

Sched.X[0] = function(object)
  object._f[1] = 1
  object._slot = nil
  object:_fire(1)
end
Sched.X[1] = function(object) object._f[1] = 0; object:_fire(2) end

function Sched.tick(object)
  if object._f[1] ~= 1 or not object._rows then return end
  local raw, id = sched.slot(object._rows, now_s())
  if id ~= object._slot then sched_apply(object, id, raw) end
end

-- ---------------------------------------------------------------------------------------------
-- Thermostat (class 18, v2). Source / Control as written by OM 1.12.1:
--   Source  {OBJ, featureIdx} | "userFeature" (global name) | "lua expression" (remote, e.g.
--           CLUx:execute(0, "SEN:get(0)")) -> evaluated as a Lua thread (may wait for the reply)
--   Control {OBJ, featureIdx} | "lua statement with %v" (remote) | nil
-- Modes (8): 0 PointValue, 1 HolidayModeValue, 2 AutoMode (Data schedule, Min..Max), 3 HeatUp
-- (PointValue + Value for Time minutes, then back). Digital output: on below Target - Hyst/2, off
-- above Target + Hyst/2 (ControlDirection 1 = cooling, mirrored). Analog output (OutputType 1):
-- 0..1 proportional over the hysteresis band. Sensor read every second (remote: every 10 s).
-- [~] Not verified against a real CLU: band placement, analog law, default State 1, output
-- forced off on Stop.
-- Events: 0 OnChange (PointValue), 1 OnStart, 2 OnStop, 3 OnOutOn, 4 OnOutOff, 5/6 Holiday on/off.
-- ---------------------------------------------------------------------------------------------
local Thermo = vclass(18, {})
local T_SRC, T_CTL, T_OUTTYPE, T_POINT, T_HOLIDAY, T_HYST, T_STATE, T_DIR, T_MODE, T_DATA,
      T_MIN, T_MAX, T_TARGET, T_OUT, T_CUR = 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14

function Thermo.init(object)
  local features = object._f
  features[T_OUTTYPE], features[T_POINT], features[T_HOLIDAY], features[T_HYST], features[T_STATE], features[T_DIR], features[T_MODE] = -1, 20, 17, 1, 1, 0, 0
  features[T_DATA], features[T_MIN], features[T_MAX], features[T_OUT] = "", 0, 30, 0
  object._next_read = 0
end

local function thermo_target(object)
  local features, mode = object._f, object._f[T_MODE]
  if mode == 1 then return tonumber(features[T_HOLIDAY]) end
  if mode == 2 and object._rows then
    return round_to(sched.value(sched.slot(object._rows, now_s()), features[T_MIN], features[T_MAX]), 1)
  end
  local point_value = tonumber(features[T_POINT]) or 20
  if mode == 3 and object._heatup then return point_value + object._heatup.add end
  return point_value
end

local function thermo_write(object, value)
  local ctl = object._f[T_CTL]
  if type(ctl) == "table" and type(ctl[1]) == "table" then
    ctl[1]:set(tonumber(ctl[2]) or 0, value)
  elseif type(ctl) == "string" and ctl ~= "" then
    local code = ctl:gsub("%%v", (value == math.floor(value)) and string.format("%d", value) or tostring(value))
    local fn, err = load(code, "=" .. object._name .. ".Control", "t")
    if not fn then clu.log(1, object._name .. ": bad Control: " .. tostring(err)); return end
    __spawn(object._name .. " Control", fn)
  end
end

local function thermo_output(object, value)
  local features = object._f
  if value == features[T_OUT] and object._written then return end
  local was = tonumber(features[T_OUT]) or 0
  features[T_OUT], object._written = value, true
  thermo_write(object, value)
  if value > 0 and was <= 0 then object:_fire(3) elseif value <= 0 and was > 0 then object:_fire(4) end
end

local function thermo_control(object)
  local features = object._f
  local current_value, target_value = tonumber(features[T_CUR]), thermo_target(object)
  features[T_TARGET] = target_value
  if features[T_STATE] ~= 1 or not current_value or not target_value then return end
  local hyst = math.max(tonumber(features[T_HYST]) or 1, 0)
  local err = target_value - current_value -- > 0: too cold
  if features[T_DIR] == 1 then err = -err end
  if features[T_OUTTYPE] == 1 then
    local out = hyst > 0 and (err + hyst / 2) / hyst or (err > 0 and 1 or 0)
    thermo_output(object, round_to(math.max(0, math.min(1, out)), 3))
  else
    local on = features[T_OUT] > 0
    if err >= hyst / 2 and (err > 0 or hyst > 0) then on = true elseif err <= -hyst / 2 then on = false end
    thermo_output(object, on and 1 or 0)
  end
end

local function thermo_read(object)
  local src = object._f[T_SRC]
  if type(src) == "table" and type(src[1]) == "table" then
    object._f[T_CUR] = tonumber(src[1]:get(tonumber(src[2]) or 0))
  elseif type(src) == "string" and src:match("^[%a_][%w_]*$") then
    object._f[T_CUR] = tonumber(type(getVar) == "function" and getVar(src) or _G[src])
  elseif type(src) == "string" and src ~= "" then
    if object._reading then return end
    local fn, err = load("return " .. src, "=" .. object._name .. ".Source", "t")
    if not fn then clu.log(1, object._name .. ": bad Source: " .. tostring(err)); return end
    object._reading = true
    __spawn(object._name .. " Source", function()
      local ok, value = pcall(fn)
      object._reading = false
      if ok and tonumber(value) then object._f[T_CUR] = tonumber(value); thermo_control(object) end
    end)
    return "async"
  end
end

Thermo.S[T_SRC] = function(object, value) object._f[T_SRC] = value; object._next_read = 0 end
Thermo.S[T_CTL] = function(object, value) object._f[T_CTL] = value; object._written = false end
Thermo.S[T_POINT] = function(object, value)
  value = tonumber(value)
  if not value or value == object._f[T_POINT] then return end
  object._f[T_POINT] = value
  object:_fire(0)
  thermo_control(object)
end
Thermo.S[T_STATE] = function(object, value)
  if (tonumber(value) or 0) ~= 0 then Thermo.X[0](object) else Thermo.X[1](object) end
end
Thermo.S[T_MODE] = function(object, value)
  value = math.tointeger(tonumber(value))
  if value == nil or value < 0 or value > 3 then return end
  local was = object._f[T_MODE]
  object._f[T_MODE] = value
  if value == 1 and was ~= 1 then object:_fire(5) elseif value ~= 1 and was == 1 then object:_fire(6) end
  thermo_control(object)
end
Thermo.S[T_DATA] = function(object, value)
  object._f[T_DATA] = tostring(value or "")
  object._rows = sched.decode(object._f[T_DATA])
  if object._f[T_DATA] ~= "" and not object._rows then clu.log(2, object._name .. ": bad schedule Data") end
end
Thermo.S[T_OUTTYPE] = function(object, value) object._f[T_OUTTYPE] = tonumber(value) or -1 end
for _, i in ipairs({ T_TARGET, T_OUT, T_CUR }) do Thermo.S[i] = function() end end -- read-only

Thermo.X[0] = function(object) -- Start
  if object._f[T_STATE] == 1 and object._started then return end
  object._f[T_STATE], object._started = 1, true
  object:_fire(1)
  thermo_control(object)
end
Thermo.X[1] = function(object) -- Stop: output off
  if object._f[T_STATE] == 0 then return end
  object._f[T_STATE] = 0
  thermo_output(object, 0)
  object:_fire(2)
end
Thermo.X[2] = function(object) Thermo.S[T_POINT](object, (tonumber(object._f[T_POINT]) or 20) + 1) end
Thermo.X[3] = function(object) Thermo.S[T_POINT](object, (tonumber(object._f[T_POINT]) or 20) - 1) end
Thermo.X[4] = function(object, value, minutes) -- HeatUp(Value 0..15 °C, Time min)
  value, minutes = tonumber(value) or 0, tonumber(minutes) or 0
  if minutes <= 0 then return end
  local back = object._f[T_MODE] == 3 and object._heatup and object._heatup.back or object._f[T_MODE]
  object._heatup = { add = math.max(0, math.min(15, value)), till = clu.millis() + minutes * 60000, back = back }
  Thermo.S[T_MODE](object, 3)
end
Thermo.X[5] = function(object) Thermo.S[T_MODE](object, 1) end
Thermo.X[6] = function(object) if object._f[T_MODE] == 1 then Thermo.S[T_MODE](object, 0) end end
Thermo.X[7] = function(object) Thermo.S[T_MODE](object, 2) end
Thermo.X[8] = function(object) if object._f[T_MODE] == 2 then Thermo.S[T_MODE](object, 0) end end

function Thermo.tick(object, now)
  if object._f[T_MODE] == 3 and object._heatup and now >= object._heatup.till then
    local back = object._heatup.back
    object._heatup = nil
    Thermo.S[T_MODE](object, back)
  end
  if now < object._next_read then return end
  local async = thermo_read(object)
  object._next_read = now + (async and 10000 or 1000)
  if not async then thermo_control(object) end
end

-- ---------------------------------------------------------------------------------------------
-- SunriseSunsetCalendar (class 39, v3). Sun times from the NOAA/Almanac sunrise equation
-- (zenith 90.833°, accuracy ~1-2 min, the interface promises ±5). Features: 0 Longitude,
-- 1 Latitude, 2 State, 3/4 SunriseUTC/SunsetUTC "HH:MM", 5/6 local "HH:MM", 7..10 timestamps
-- (UTC epoch s; "local" = UTC epoch + UTC offset), 11 IsDayNow, 12/13 offsets [min],
-- 14/15 minutes to the next sunrise/sunset (offset included). Unavailable (polar day/night):
-- "N\A" / -1. Events: 0 OnStart, 1 OnStop, 2 OnSunrise, 3 OnSunset, 4 OnSunriseSunsetChange,
-- 5 OnDay, 6 OnNight (IsDayNow change, no offsets). Checked once per wall-clock minute.
-- [~] string format "HH:MM" and the local-timestamp convention are guesses.
-- ---------------------------------------------------------------------------------------------
local Sun = vclass(39, {})
local rad, deg = math.pi / 180, 180 / math.pi

local function days_from_civil(year, month, day) -- days since 1970-01-01 (proleptic Gregorian)
  year = month <= 2 and year - 1 or year
  local era = (year >= 0 and year or year - 399) // 400
  local yoe = year - era * 400
  local doy = (153 * (month + (month > 2 and -3 or 9)) + 2) // 5 + day - 1
  local doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
  return era * 146097 + doe - 719468
end

-- UTC epoch of sunrise (rising) / sunset on calendar day year-month-day at lat/lon; nil when the sun does
-- not rise / set that day.
local function sun_event(year, month, day, latitude, longitude, rising)
  local day_of_year = days_from_civil(year, month, day) - days_from_civil(year, 1, 1) + 1
  local longitude_hour = longitude / 15
  local approx_time = day_of_year + ((rising and 6 or 18) - longitude_hour) / 24
  local mean_anomaly = 0.9856 * approx_time - 3.289
  local true_longitude = (mean_anomaly + 1.916 * math.sin(mean_anomaly * rad) +
                          0.020 * math.sin(2 * mean_anomaly * rad) + 282.634) % 360
  local right_ascension = (math.atan(0.91764 * math.tan(true_longitude * rad)) * deg) % 360
  right_ascension = (right_ascension + (true_longitude // 90) * 90 - (right_ascension // 90) * 90) / 15
  local sin_dec = 0.39782 * math.sin(true_longitude * rad)
  local cos_dec = math.cos(math.asin(sin_dec))
  local cos_h = (math.cos(90.833 * rad) - sin_dec * math.sin(latitude * rad)) / (cos_dec * math.cos(latitude * rad))
  if cos_h > 1 or cos_h < -1 or cos_h ~= cos_h then return nil end
  local hour_angle = math.acos(cos_h) * deg
  if rising then hour_angle = 360 - hour_angle end
  local local_mean_time = hour_angle / 15 + right_ascension - 0.06571 * approx_time - 6.622
  local utc_hour = (local_mean_time - longitude_hour) % 24
  -- The event belongs to the local solar day y-m-d: its UTC hour is near 6/18 h - lon/15, which can
  -- fall on the previous/next UTC day (east: Sydney sunrise ~21 UTC the day before).
  local expected_utc_hour = (rising and 6 or 18) - longitude_hour
  if utc_hour - expected_utc_hour > 12 then utc_hour = utc_hour - 24
  elseif expected_utc_hour - utc_hour > 12 then utc_hour = utc_hour + 24 end
  return math.floor(days_from_civil(year, month, day) * 86400 + utc_hour * 3600 + 0.5)
end
__sun_event = sun_event

local function utc_offset(ts)
  local timezone_text = os.date("%z", ts)
  local sign, hours, minutes = tostring(timezone_text):match("^([+-])(%d%d)(%d%d)$")
  if not sign then return 0 end
  local offset_seconds = tonumber(hours) * 3600 + tonumber(minutes) * 60
  return sign == "-" and -offset_seconds or offset_seconds
end

local function sun_day(object, ts) -- sunrise/sunset for the local calendar day of ts (cached)
  local date = os.date("*t", ts)
  local lat, lon = tonumber(object._f[1]) or 0, tonumber(object._f[0]) or 0
  local key = string.format("%d-%d-%d/%s/%s", date.year, date.month, date.day, lat, lon)
  object._days = object._days or {}
  local day_events = object._days[key]
  if not day_events then
    if lat < -90 or lat > 90 or lon < -180 or lon > 180 then
      day_events = {}
    else
      day_events = { rise = sun_event(date.year, date.month, date.day, lat, lon, true),
                     set = sun_event(date.year, date.month, date.day, lat, lon, false) }
    end
    object._ndays = (object._ndays or 0) + 1
    if object._ndays > 8 then object._days, object._ndays = {}, 1 end -- yesterday .. +2 days
    object._days[key] = day_events
  end
  return day_events
end

local function sun_next(object, ts, which, offset_idx)
  local off = (tonumber(object._f[offset_idx]) or 0) * 60
  for dd = -1, 2 do -- -1: CLU clock zone far from the location (its sunset after local midnight)
    local event_time = sun_day(object, ts + dd * 86400)[which]
    if event_time and event_time + off > ts then return event_time + off end
  end
  return nil
end

local function hhmm(ts, utc) return ts and os.date(utc and "!%H:%M" or "%H:%M", ts) or "N\\A" end

local function sun_update(object, now)
  local features, todays_events = object._f, sun_day(object, now)
  features[3], features[4] = hhmm(todays_events.rise, true), hhmm(todays_events.set, true)
  features[5], features[6] = hhmm(todays_events.rise), hhmm(todays_events.set)
  features[7], features[8] = todays_events.rise or -1, todays_events.set or -1
  features[9] = todays_events.rise and todays_events.rise + utc_offset(todays_events.rise) or -1
  features[10] = todays_events.set and todays_events.set + utc_offset(todays_events.set) or -1
  if todays_events.rise and todays_events.set then
    features[11] = 0
    for dd = -1, 1 do
      local day_events = sun_day(object, now + dd * 86400)
      if day_events.rise and day_events.set and now >= day_events.rise and now < day_events.set then features[11] = 1 end
    end
  else
    features[11] = -1
  end
  local next_sunrise, next_sunset = sun_next(object, now, "rise", 12), sun_next(object, now, "set", 13)
  features[14] = next_sunrise and math.ceil((next_sunrise - now) / 60) or -1
  features[15] = next_sunset and math.ceil((next_sunset - now) / 60) or -1
end

function Sun.init(object)
  object._f[0], object._f[1], object._f[2], object._f[12], object._f[13] = 0, 0, 1, 0, 0
  object._dirty = true
end
for i = 3, 11 do Sun.S[i] = function() end end -- read-only
Sun.S[14], Sun.S[15] = function() end, function() end
for _, i in ipairs({ 0, 1, 12, 13 }) do
  Sun.S[i] = function(object, value) object._f[i] = tonumber(value) or 0; object._dirty = true end
end
Sun.S[2] = function(object, value) if (tonumber(value) or 0) ~= 0 then Sun.X[0](object) else Sun.X[1](object) end end
Sun.X[0] = function(object)
  if object._f[2] == 1 and object._started then return end
  object._f[2], object._started, object._last = 1, true, nil
  object:_fire(0)
end
Sun.X[1] = function(object)
  if object._f[2] == 0 then return end
  object._f[2] = 0
  object:_fire(1)
end
for i = 3, 15 do
  Sun.G[i] = function(object)
    if object._dirty then object._dirty = false; sun_update(object, now_s()) end
    return object._f[i]
  end
end

function Sun.tick(object)
  local now = now_s()
  if object._last and now // 60 == object._last // 60 and not object._dirty then return end
  local prev, prev_day = object._last, object._f[11]
  sun_update(object, now)
  object._dirty, object._last = false, now
  if object._f[2] ~= 1 or not prev then return end
  -- events whose (offset) time passed since the previous check
  for _, e in ipairs({ { "rise", 12, 2 }, { "set", 13, 3 } }) do
    local at = sun_next(object, prev, e[1], e[2])
    if at and at <= now then
      object:_fire(e[3])
      object:_fire(4)
    end
  end
  if prev_day ~= object._f[11] and prev_day ~= -1 and object._f[11] ~= -1 then
    object:_fire(object._f[11] == 1 and 5 or 6)
  end
end

-- ---------------------------------------------------------------------------------------------
-- Remote CLU (class 1): proxies to the CLU at o._ip over UDP (project key).
--   execute(0, "lua")        -> runs the script on the remote CLU, returns its result
--   execute(i, …)            -> "<NAME>:execute(i, …)" on the remote CLU, returns its result
--   get(i)                    -> "<NAME>:get(i)" on the remote CLU (value cached in _f)
--   set(i, v)                 -> "<NAME>:set(i, v)" on the remote CLU (fire-and-forget)
-- As on the real CLU: the call yields the calling Lua thread (event handlers
-- run as coroutines) until the reply. Result "true"/"false"/"nil"/number/string; timeout or send
-- failure -> "error". Outside a yieldable context (req:, MAIN.LUA top level) the script is still
-- sent but the call returns nil at once (get: the cached value). Real CLU get/set on class 1
-- objects is not documented: get/set via NAME:get/NAME:set is our choice.
-- ---------------------------------------------------------------------------------------------
local Remote = vclass(1, {})

local function parse_result(result_text)
  if result_text == nil then return "error" end
  if result_text == "nil" then return nil elseif result_text == "true" then return true elseif result_text == "false" then return false end
  local parsed_number = tonumber(result_text)
  if parsed_number ~= nil then return parsed_number end
  return result_text
end

-- Returns: sent, result (result only when the caller could wait for it).
local function remote(object, script)
  local id = clu.remote(object._ip, script)
  if not id then return false, "error" end
  local did_wait, result_text = __wait(id)
  if not did_wait then return true, nil end
  return true, parse_result(result_text)
end

-- Lua source literal for a proxied argument. lit() (clientReport format) only escapes '"', so
-- backslashes, newlines or NULs in strings would break or alter the remote script; ser() is %q-based.
local function rlit(value) return ser(value) or "nil" end

function Remote:set(idx, value)
  self._f[idx] = value
  clu.remote(self._ip, self._name .. ":set(" .. tostring(idx) .. ", " .. rlit(value) .. ")")
end

function Remote:get(idx)
  if not coroutine.isyieldable() then return self._f[idx] end
  local sent, result = remote(self, self._name .. ":get(" .. tostring(idx) .. ")")
  if sent and result ~= "error" then self._f[idx] = result end
  return result
end

function Remote:execute(idx, ...)
  if idx == 0 then
    local script = ...
    local sent, result = remote(self, tostring(script))
    return result
  end
  local args = { tostring(idx) }
  for i = 1, select("#", ...) do args[#args + 1] = rlit((select(i, ...))) end
  local sent, result = remote(self, self._name .. ":execute(" .. table.concat(args, ", ") .. ")")
  return result
end
