-- Virtual CLU objects (declared in OM.LUA as OBJECT:new(class, "NAME")) and the remote CLU proxy
-- (OBJECT:new(1, ip, "NAME")). Loaded after prelude.lua. Interfaces: docs/device-interfaces/object_*.xml.
--
-- Each class: cls.init(o), cls.G[idx](o), cls.S[idx](o, v), cls.X[idx](o, ...), cls.tick(o, now_ms).
-- Features without a getter/setter fall back to the generic store o._f.
local C = __clu
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
    local g = def.G[idx]
    if g then return g(self) end
    return self._f[idx]
  end

  function def:set(idx, v)
    local s = def.S[idx]
    if s then s(self, v) else self._f[idx] = v end
  end

  function def:execute(idx, ...)
    local e = def.X[idx]
    if e then return e(self, ...) end
    -- execute(i, v) on a settable feature behaves like set (OM uses both forms)
    if def.S[idx] then return def.S[idx](self, ...) end
    C.log(2, self._name .. ":execute(" .. tostring(idx) .. ") not implemented")
  end

  VCLASS[id] = def
  return def
end

local function now_s() return os.time() end

-- ---------------------------------------------------------------------------------------------
-- Persistent store: one file on the FAT data partition (wear-levelled, 6 MB), default
-- a:\STORAGE.DAT. Content is a Lua table literal {section = {key = value}}. Writes are
-- debounced (PERSIST_DELAY_MS) to save flash; flush(true) writes now. Save = write .TMP, remove
-- .DAT, rename; load falls back to .TMP if a power cut hit between remove and rename.
-- ---------------------------------------------------------------------------------------------
local PERSIST_DELAY_MS = 5000
local persist = { data = nil, dirty = false, due = nil }
local persist_path = C.storage or ((C.root or ".") .. "/STORAGE.DAT")
local persist_tmp = persist_path:gsub("%.%w+$", "") .. ".TMP"

-- Serialize nil/boolean/number/string and tables of them (no functions/userdata/cycles).
local function ser(v, depth)
  local t = type(v)
  if t == "nil" or t == "boolean" then return tostring(v)
  elseif t == "number" then
    if math.type(v) == "integer" then return string.format("%d", v) end
    if v ~= v then return "0/0" elseif v == math.huge then return "1/0" elseif v == -math.huge then return "-1/0" end
    local f = string.format("%.17g", v)
    return f:find("[%.eEn]") and f or f .. ".0"
  elseif t == "string" then return string.format("%q", v)
  elseif t == "table" then
    depth = (depth or 0) + 1
    if depth > 16 then return nil end
    local parts = {}
    for k, x in pairs(v) do
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
  local f = io.open(path, "rb")
  if not f then return nil end
  local txt = f:read("a")
  f:close()
  return txt
end

function persist.load()
  if persist.data then return persist.data end
  persist.data = {}
  for _, path in ipairs({ persist_path, persist_tmp }) do
    local txt = read_file(path)
    if txt then
      local fn = load("return " .. txt, "=storage", "t", {})
      local ok, d = pcall(fn or error)
      if ok and type(d) == "table" then
        persist.data = d
        break
      end
      C.log(1, "storage: " .. path .. " unreadable, ignored")
    end
  end
  return persist.data
end

function persist.section(name)
  local d = persist.load()
  d[name] = d[name] or {}
  return d[name]
end

function persist.mark(now)
  persist.dirty = true
  persist.due = persist.due or ((now or C.millis()) + PERSIST_DELAY_MS)
end

function persist.size() return #(ser(persist.load()) or "") end

function persist.flush(force, now)
  if not persist.dirty then return true end
  if not force and (now or C.millis()) < persist.due then return true end
  local txt = ser(persist.load()) or "{}"
  local f = io.open(persist_tmp, "wb")
  local ok = f and f:write(txt) and f:close()
  if ok then
    os.remove(persist_path)
    ok = os.rename(persist_tmp, persist_path)
  end
  if not ok then
    C.log(1, "storage: write " .. persist_path .. " failed")
    persist.due = (now or C.millis()) + PERSIST_DELAY_MS -- retry later
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

local function cron_field(s, lo, hi)
  local set, any = {}, s == "*"
  for part in s:gmatch("[^,]+") do
    local range, step = part:match("^([^/]+)/(%d+)$")
    range = range or part
    step = tonumber(step) or 1
    if step < 1 then return nil end
    local a, b
    if range == "*" then
      a, b = lo, hi
    else
      a, b = range:match("^(%d+)-(%d+)$")
      if a then
        a, b = tonumber(a), tonumber(b)
      else
        a = tonumber(range:match("^(%d+)$"))
        if not a then return nil end
        b = (part:find("/")) and hi or a
      end
    end
    if a < lo or b > hi or a > b then return nil end
    for v = a, b, step do set[v] = true end
  end
  return set, any
end

function cron.parse(rule)
  if type(rule) ~= "string" then return nil end
  local f = {}
  for w in rule:gmatch("%S+") do f[#f + 1] = w end
  if #f ~= 5 then return nil end
  local r = {}
  local any
  r.min = cron_field(f[1], 0, 59)
  r.hour = cron_field(f[2], 0, 23)
  r.dom, r.dom_any = cron_field(f[3], 1, 31)
  r.mon = cron_field(f[4], 1, 12)
  r.dow, any = cron_field(f[5], 0, 7)
  r.dow_any = any
  if not (r.min and r.hour and r.dom and r.mon and r.dow) then return nil end
  if r.dow[7] then r.dow[0] = true end
  return r
end

local function day_match(r, t)
  local dom, dow = r.dom[t.day], r.dow[t.wday - 1]
  if r.dom_any and r.dow_any then return true end
  if r.dom_any then return dow end
  if r.dow_any then return dom end
  return dom or dow -- classic cron: either restricted field matches
end

function cron.match(r, ts)
  local t = os.date("*t", ts)
  return r.min[t.min] and r.hour[t.hour] and r.mon[t.month] and day_match(r, t) or false
end

-- First matching minute strictly after ts (nil if none within ~4 years, e.g. "0 0 31 2 *").
function cron.next(r, ts)
  local t = os.date("*t", ts - ts % 60 + 60)
  t.sec = 0
  for _ = 1, 4 * 366 * 24 + 60 * 24 do
    if not r.mon[t.month] then
      t.month, t.day, t.hour, t.min = t.month + 1, 1, 0, 0
    elseif not day_match(r, t) then
      t.day, t.hour, t.min = t.day + 1, 0, 0
    elseif not r.hour[t.hour] then
      t.hour, t.min = t.hour + 1, 0
    elseif not r.min[t.min] then
      t.min = t.min + 1
      if t.min > 59 then t.hour, t.min = t.hour + 1, 0 end
    else
      return os.time(t)
    end
    t = os.date("*t", os.time(t)) -- normalise (fills wday, rolls over)
  end
  return nil
end

__cron = cron

-- cron.next cached per wall-clock minute (features polled by clientReport every loop).
local function next_cached(o, key, fn)
  local m = os.time() // 60
  local c = o._nc
  if not c or c.m ~= m then c = { m = m }; o._nc = c end
  if c[key] == nil then c[key] = { fn() } end
  return c[key][1]
end

-- Minute-boundary helper for cron-driven objects: true once per wall-clock minute.
local function new_minute(o)
  local m = now_s() // 60
  if o._minute == m then return false end
  local first = o._minute == nil
  o._minute = m
  return not first
end

-- ---------------------------------------------------------------------------------------------
-- Timer (class 6): Time[ms], Mode 0 countdown / 1 interval, State 0 stopped / 1 counting / 2 paused,
-- Value = ms left. Events: 0 OnTimer, 1 OnStart, 2 OnStop, 3 OnPause.
-- ---------------------------------------------------------------------------------------------
local Timer = vclass(6, {})

function Timer.init(o)
  o._f[0], o._f[1] = 1000, 0
  o._state, o._left = 0, 1000
end

Timer.G[2] = function(o) return o._state end
Timer.G[3] = function(o)
  if o._state == 0 then return 0 end
  return math.max(0, math.floor(o._left))
end
Timer.S[0] = function(o, v)
  o._f[0] = tonumber(v) or o._f[0]
  if o._state == 1 then o._left, o._last = o._f[0], C.millis() end
end
Timer.S[1] = function(o, v) o._f[1] = (tonumber(v) or 0) ~= 0 and 1 or 0 end

Timer.X[0] = function(o) -- Start
  o._left, o._last, o._state = tonumber(o._f[0]) or 1000, C.millis(), 1
  o:_fire(1)
end
Timer.X[1] = function(o) -- Stop
  o._state, o._left = 0, tonumber(o._f[0]) or 1000
  o:_fire(2)
end
Timer.X[2] = function(o) -- Pause: pauses a running timer, resumes a paused one
  if o._state == 1 then
    o._state = 2
    o:_fire(3)
  elseif o._state == 2 then
    o._state, o._last = 1, C.millis()
  end
end

function Timer.tick(o, now)
  if o._state ~= 1 then return end
  o._left = o._left - (now - o._last)
  o._last = now
  if o._left > 0 then return end
  local period = tonumber(o._f[0]) or 1000
  if o._f[1] == 1 and period > 0 then
    o._left = o._left % period
    if o._left == 0 then o._left = period end
  else
    -- Countdown end: the timer stops (OnStop), then OnTimer, like the vCLU (Timer.ts).
    -- UNVERIFIED on a real CLU.
    o._state, o._left = 0, period
    o:_fire(2)
  end
  o:_fire(0)
end

-- ---------------------------------------------------------------------------------------------
-- Calendar (class 15): Rule (cron), SinceLastRun / ToNextRun [min], State.
-- Events: 0 OnCalendar, 1 OnStart, 2 OnStop, 3 OnCancel.
-- ---------------------------------------------------------------------------------------------
local Calendar = vclass(15, {})

function Calendar.init(o)
  o._f[0], o._f[3] = "", 1
  o._cancel = 0
end

Calendar.S[0] = function(o, v)
  o._nc = nil
  o._rule = cron.parse(v)
  o._f[0] = o._rule and v or "ERROR"
end
Calendar.S[3] = function() end -- read-only
Calendar.G[1] = function(o)
  if not o._lastrun then return 0 end
  return (now_s() - o._lastrun) // 60
end
Calendar.G[2] = function(o)
  if not o._rule or o._f[3] ~= 1 then return 0 end
  local n = next_cached(o, "n", function() return cron.next(o._rule, now_s()) end)
  return n and (n - now_s() + 59) // 60 or 0
end
Calendar.X[0] = function(o) o._f[3] = 1; o:_fire(1) end
Calendar.X[1] = function(o) o._f[3] = 0; o:_fire(2) end
Calendar.X[3] = function(o, n)
  o._cancel = o._cancel + (tonumber(n) or 1)
  o:_fire(3)
end

function Calendar.tick(o)
  if not new_minute(o) or not o._rule or o._f[3] ~= 1 then return end
  local t = now_s()
  if not cron.match(o._rule, t) then return end
  if o._cancel > 0 then
    o._cancel = o._cancel - 1
    return
  end
  o._lastrun = t
  o:_fire(0)
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

local function es_fmt(r) return "{" .. r.id .. "," .. r.state .. "," .. lit(r.cron) .. "}" end

local function es_sorted(o)
  local l = {}
  for _, r in pairs(o._rules) do l[#l + 1] = r end
  table.sort(l, function(a, b) return a.id < b.id end)
  return l
end

local function es_next(o)
  local best, bt = nil, nil
  for _, r in ipairs(es_sorted(o)) do
    if r.state == 1 then
      local t = cron.next(r.parsed, now_s())
      if t and (not bt or t < bt) then best, bt = r, t end
    end
  end
  return best
end

local function es_save(o)
  local l = {}
  for _, r in ipairs(es_sorted(o)) do l[#l + 1] = { r.id, r.state, r.cron } end
  persist.section("es")[o._name] = l
  persist.mark()
end

function ES.init(o)
  o._f[5] = 1
  o._rules = {}
  for _, r in ipairs(persist.section("es")[o._name] or {}) do
    local id, state, rule = tonumber(r[1]), tonumber(r[2]), r[3]
    local parsed = type(rule) == "string" and cron.parse(rule)
    if id and parsed and id >= 1 and id <= ES_MAX then
      o._rules[id] = { id = id, state = state == 0 and 0 or 1, cron = rule, parsed = parsed }
    end
  end
end

ES.G[0] = function(o)
  local p = {}
  for _, r in ipairs(es_sorted(o)) do p[#p + 1] = es_fmt(r) end
  return "{" .. table.concat(p, ",") .. "}"
end
ES.G[1] = function(o)
  if o._current and now_s() - o._current_t < 60 then return es_fmt(o._current) end
  return "N\\A"
end
ES.G[2] = function(o)
  local r = next_cached(o, "n", function() return es_next(o) end)
  return r and es_fmt(r) or "N\\A"
end
ES.G[3] = function(o) return #es_sorted(o) end
ES.G[4] = function(o) return ES_MAX - #es_sorted(o) end
ES.S[5] = function(o, v)
  local s = (tonumber(v) or 0) ~= 0 and 1 or 0
  if s == o._f[5] then return end
  o._f[5] = s
  o:_fire(s == 1 and 0 or 1)
end

ES.X[0] = function(o) ES.S[5](o, 1) end
ES.X[1] = function(o) ES.S[5](o, 0) end
ES.X[2] = function(o, rule) -- AddRule -> id, 0 on error
  local parsed = cron.parse(rule)
  if not parsed then return 0 end
  for _, r in pairs(o._rules) do
    if r.cron == rule then return r.id end
  end
  for id = 1, ES_MAX do
    if not o._rules[id] then
      o._rules[id] = { id = id, state = 1, cron = rule, parsed = parsed }
      o._nc = nil
      es_save(o)
      o:_fire(3)
      return id
    end
  end
  return 0
end
ES.X[3] = function(o, id) -- DeleteRule -> 0 ok, 1 error
  id = tonumber(id)
  if not id or not o._rules[id] then return 1 end
  o._rules[id] = nil
  o._nc = nil
  es_save(o)
  o:_fire(4)
  return 0
end
ES.X[4] = function(o, id)
  local r = o._rules[tonumber(id) or -1]
  return r and es_fmt(r) or "N\\A"
end
local function es_state(o, id, s)
  local r = o._rules[tonumber(id) or -1]
  if not r then return 1 end
  if r.state ~= s then
    r.state = s
    es_save(o)
  end
  o._nc = nil
  return 0
end
ES.X[5] = function(o, id) return es_state(o, id, 1) end
ES.X[6] = function(o, id) return es_state(o, id, 0) end
ES.X[7] = function(o) return ES.G[0](o) end
ES.X[8] = function(o) return ES.G[2](o) end
ES.X[9] = function(o)
  local had = next(o._rules) ~= nil
  o._rules = {}
  o._nc = nil
  if had then
    es_save(o)
    o:_fire(4)
  end
  return 0
end

function ES.tick(o)
  if not new_minute(o) or o._f[5] ~= 1 then return end
  local t = now_s()
  for _, r in ipairs(es_sorted(o)) do
    if r.state == 1 and cron.match(r.parsed, t) then
      o._current, o._current_t = r, t
      o:_fire(2)
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

function PS.init(o)
  o._f[0], o._f[1], o._f[2], o._f[4], o._f[5], o._f[6] = 60, 1, 0, 2, 0, 0
  o._last = C.millis()
  o._ignore_until = 0
  o._holding = false
end

local function ps_on(o)
  if o._f[2] == 0 then
    o._f[2] = 1
    o:_fire(2)
  end
end

local function ps_off(o)
  if o._f[2] == 1 then
    o._f[2] = 0
    o:_fire(3)
  end
end

local function ps_lock(o, v)
  v = (tonumber(v) or 0) ~= 0 and 1 or 0
  if v == 1 then
    o._last = C.millis()
    if o._f[5] == 0 then
      o._f[5] = 1
      ps_on(o)
    end
  elseif o._f[5] == 1 then
    o._f[5] = 0
    o._ignore_until = C.millis() + (tonumber(o._f[4]) or 0) * 1000
    ps_off(o)
  end
end

PS.G[3] = function(o)
  if o._holding then return 0 end
  return (C.millis() - o._last) // 1000
end
PS.S[1] = function(o, v)
  local s = (tonumber(v) or 0) ~= 0 and 1 or 0
  if s == o._f[1] then return end
  o._f[1] = s
  o:_fire(s == 1 and 0 or 1)
end
PS.S[2] = function() end -- read-only
PS.S[5] = ps_lock
PS.X[0] = function(o) PS.S[1](o, 1) end
PS.X[1] = function(o) PS.S[1](o, 0) end
PS.X[2] = function(o) -- DetectPresence
  local now = C.millis()
  o._last = now
  if o._f[6] == 1 then o._holding = true end
  if o._f[1] ~= 1 or o._f[5] == 1 or now < o._ignore_until then return end
  ps_on(o)
end
PS.X[3] = function(o) -- UndetectPresence
  if o._holding then
    o._holding = false
    o._last = C.millis()
  end
end
PS.X[4] = function(o) ps_lock(o, o._f[5] == 1 and 0 or 1) end

function PS.tick(o, now)
  if o._f[2] ~= 1 or o._f[5] == 1 or o._holding or o._f[1] ~= 1 then return end
  if now - o._last >= (tonumber(o._f[0]) or 60) * 1000 then ps_off(o) end
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

function Stat.init(o)
  o._f[0], o._f[1], o._f[2], o._f[6] = 0, 0, 0, 0
  for _, i in ipairs({ 4, 5, 7, 8, 9 }) do o._f[i] = -1 end
end

Stat.S[0] = function(o, v)
  v = tonumber(v) or 0
  if v ~= o._f[0] then
    o._f[0] = v
    o:_fire(0)
  end
end

local Pulse = vclass(48, {})

function Pulse.init(o)
  o._f[10], o._f[1], o._f[2], o._f[3] = 0, 0, 0, -1
end

Pulse.S[10] = function(o, v) o._f[10] = tonumber(v) or 0 end

Pulse.X[0] = function(o) -- TriggerPulse
  o._f[10] = o._f[10] + 1
  o:_fire(1)
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

local function sto_assign(name, v)
  if type(setVar) == "function" then setVar(name, v) else _G[name] = v end
end

local function sto_capture(name)
  local vars = sto_vars()
  local cur = _G[name]
  if ser(cur) == nil then
    C.log(2, "storage: " .. name .. " has unsupported type " .. type(cur))
    return false
  end
  if ser(cur) ~= ser(vars[name]) or vars[name] == nil then
    vars[name] = cur
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

function Storage.init(o)
  if storage_hooked then return end
  storage_hooked = true
  HOOKS.init[#HOOKS.init + 1] = function()
    for name, v in pairs(sto_vars()) do sto_assign(name, v) end
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

Storage.X[4] = function(o, name) -- Store
  if type(name) == "string" and name ~= "" then
    sto_capture(name)
  else
    for n in pairs(sto_vars()) do sto_capture(n) end
  end
  if persist.size() > STORAGE_BUDGET then C.log(2, "storage: over budget") end
  persist.flush(true)
end

-- ---------------------------------------------------------------------------------------------
-- Push (class 31): no Grenton cloud here; Send logs the notification and fires OnSend.
-- Features: 0 Message, 1 Title, 2 LastSendTime, 3 Interval [s]. Events: 0 OnSend, 1 OnOverflow.
-- ---------------------------------------------------------------------------------------------
local Push = vclass(31, {})
local PUSH_MAX = 500

function Push.init(o)
  o._f[0], o._f[1], o._f[2], o._f[3] = "", "", "", 1
end

local function push_append(o, idx, v)
  local s = o._f[idx] .. tostring(v or "")
  if #s > PUSH_MAX then
    s = s:sub(1, PUSH_MAX)
    o:_fire(1)
  end
  o._f[idx] = s
end

Push.S[0] = function(o, v) push_append(o, 0, v) end
Push.S[1] = function(o, v) push_append(o, 1, v) end
Push.S[2] = function() end
Push.X[1] = function(o) o._f[0] = "" end
Push.X[3] = function(o) o._f[1] = "" end
Push.X[4] = function(o)
  local now = C.millis()
  if o._sent and now - o._sent < (tonumber(o._f[3]) or 1) * 1000 then return end
  o._sent = now
  o._f[2] = os.date("%Y-%m-%d %H:%M:%S")
  C.log(3, "PUSH " .. o._name .. ": [" .. o._f[1] .. "] " .. o._f[0])
  o:_fire(0)
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
      local v = math.min(rows[d][q], SCHED_SCALE - 1)
      local hi, lo = v // 90 + 1, v % 90 + 1
      out[#out + 1] = SCHED_ALPHA:sub(hi, hi) .. SCHED_ALPHA:sub(lo, lo)
    end
  end
  return table.concat(out) .. "+00:zz"
end

-- Raw slot for wall-clock time ts (local time), plus the slot's start (seconds).
function sched.slot(rows, ts)
  local t = os.date("*t", ts)
  local q = t.hour * 4 + t.min // 15
  return rows[t.wday - 1][q], (t.wday - 1) * 96 + q
end

function sched.value(raw, min, max)
  min, max = tonumber(min) or 0, tonumber(max) or 1
  return min + raw * (max - min) / SCHED_SCALE
end
__sched = sched

local function round_to(v, decimals)
  local m = 10 ^ decimals
  local r = math.floor(v * m + 0.5) / m
  if decimals == 0 then return math.tointeger(r) or r end
  return r
end

-- ---------------------------------------------------------------------------------------------
-- Scheduler (class 16): Features 0 Data, 1 State, 2 Value, 3 Min, 4 Max.
-- Events: 0 OnHarmonogram (every quarter-hour while active, and on Start), 1 OnStart, 2 OnStop.
-- Value rounded like OM's editor shows it: Max <= 1 -> 2 decimals, <= 10 -> 1, else integer.
-- [~] firing on Start and on every slot (not only on change) is a guess from the hint text.
-- ---------------------------------------------------------------------------------------------
local Sched = vclass(16, {})

function Sched.init(o)
  o._f[0], o._f[1], o._f[2], o._f[3], o._f[4] = "", 1, 0, 0, 1
end
Sched.S[0] = function(o, v)
  o._f[0] = tostring(v or "")
  o._rows = sched.decode(o._f[0])
  if o._f[0] ~= "" and not o._rows then C.log(2, o._name .. ": bad schedule Data") end
  o._slot = nil -- re-evaluate on the next tick
end
Sched.S[1] = function() end
Sched.S[2] = function() end

local function sched_apply(o, slot_id, raw)
  local max = tonumber(o._f[4]) or 1
  local dec = max <= 1 and 2 or max <= 10 and 1 or 0
  o._slot = slot_id
  o._f[2] = round_to(sched.value(raw, o._f[3], max), dec)
  o:_fire(0)
end

Sched.X[0] = function(o)
  o._f[1] = 1
  o._slot = nil
  o:_fire(1)
end
Sched.X[1] = function(o) o._f[1] = 0; o:_fire(2) end

function Sched.tick(o)
  if o._f[1] ~= 1 or not o._rows then return end
  local raw, id = sched.slot(o._rows, now_s())
  if id ~= o._slot then sched_apply(o, id, raw) end
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

function Thermo.init(o)
  local f = o._f
  f[T_OUTTYPE], f[T_POINT], f[T_HOLIDAY], f[T_HYST], f[T_STATE], f[T_DIR], f[T_MODE] = -1, 20, 17, 1, 1, 0, 0
  f[T_DATA], f[T_MIN], f[T_MAX], f[T_OUT] = "", 0, 30, 0
  o._next_read = 0
end

local function thermo_target(o)
  local f, mode = o._f, o._f[T_MODE]
  if mode == 1 then return tonumber(f[T_HOLIDAY]) end
  if mode == 2 and o._rows then
    return round_to(sched.value(sched.slot(o._rows, now_s()), f[T_MIN], f[T_MAX]), 1)
  end
  local p = tonumber(f[T_POINT]) or 20
  if mode == 3 and o._heatup then return p + o._heatup.add end
  return p
end

local function thermo_write(o, v)
  local ctl = o._f[T_CTL]
  if type(ctl) == "table" and type(ctl[1]) == "table" then
    ctl[1]:set(tonumber(ctl[2]) or 0, v)
  elseif type(ctl) == "string" and ctl ~= "" then
    local code = ctl:gsub("%%v", (v == math.floor(v)) and string.format("%d", v) or tostring(v))
    local fn, err = load(code, "=" .. o._name .. ".Control", "t")
    if not fn then C.log(1, o._name .. ": bad Control: " .. tostring(err)); return end
    __spawn(o._name .. " Control", fn)
  end
end

local function thermo_output(o, v)
  local f = o._f
  if v == f[T_OUT] and o._written then return end
  local was = tonumber(f[T_OUT]) or 0
  f[T_OUT], o._written = v, true
  thermo_write(o, v)
  if v > 0 and was <= 0 then o:_fire(3) elseif v <= 0 and was > 0 then o:_fire(4) end
end

local function thermo_control(o)
  local f = o._f
  local cur, target = tonumber(f[T_CUR]), thermo_target(o)
  f[T_TARGET] = target
  if f[T_STATE] ~= 1 or not cur or not target then return end
  local hyst = math.max(tonumber(f[T_HYST]) or 1, 0)
  local err = target - cur -- > 0: too cold
  if f[T_DIR] == 1 then err = -err end
  if f[T_OUTTYPE] == 1 then
    local out = hyst > 0 and (err + hyst / 2) / hyst or (err > 0 and 1 or 0)
    thermo_output(o, round_to(math.max(0, math.min(1, out)), 3))
  else
    local on = f[T_OUT] > 0
    if err >= hyst / 2 and (err > 0 or hyst > 0) then on = true elseif err <= -hyst / 2 then on = false end
    thermo_output(o, on and 1 or 0)
  end
end

local function thermo_read(o)
  local src = o._f[T_SRC]
  if type(src) == "table" and type(src[1]) == "table" then
    o._f[T_CUR] = tonumber(src[1]:get(tonumber(src[2]) or 0))
  elseif type(src) == "string" and src:match("^[%a_][%w_]*$") then
    o._f[T_CUR] = tonumber(type(getVar) == "function" and getVar(src) or _G[src])
  elseif type(src) == "string" and src ~= "" then
    if o._reading then return end
    local fn, err = load("return " .. src, "=" .. o._name .. ".Source", "t")
    if not fn then C.log(1, o._name .. ": bad Source: " .. tostring(err)); return end
    o._reading = true
    __spawn(o._name .. " Source", function()
      local ok, v = pcall(fn)
      o._reading = false
      if ok and tonumber(v) then o._f[T_CUR] = tonumber(v); thermo_control(o) end
    end)
    return "async"
  end
end

Thermo.S[T_SRC] = function(o, v) o._f[T_SRC] = v; o._next_read = 0 end
Thermo.S[T_CTL] = function(o, v) o._f[T_CTL] = v; o._written = false end
Thermo.S[T_POINT] = function(o, v)
  v = tonumber(v)
  if not v or v == o._f[T_POINT] then return end
  o._f[T_POINT] = v
  o:_fire(0)
  thermo_control(o)
end
Thermo.S[T_STATE] = function(o, v)
  if (tonumber(v) or 0) ~= 0 then Thermo.X[0](o) else Thermo.X[1](o) end
end
Thermo.S[T_MODE] = function(o, v)
  v = math.tointeger(tonumber(v))
  if v == nil or v < 0 or v > 3 then return end
  local was = o._f[T_MODE]
  o._f[T_MODE] = v
  if v == 1 and was ~= 1 then o:_fire(5) elseif v ~= 1 and was == 1 then o:_fire(6) end
  thermo_control(o)
end
Thermo.S[T_DATA] = function(o, v)
  o._f[T_DATA] = tostring(v or "")
  o._rows = sched.decode(o._f[T_DATA])
  if o._f[T_DATA] ~= "" and not o._rows then C.log(2, o._name .. ": bad schedule Data") end
end
Thermo.S[T_OUTTYPE] = function(o, v) o._f[T_OUTTYPE] = tonumber(v) or -1 end
for _, i in ipairs({ T_TARGET, T_OUT, T_CUR }) do Thermo.S[i] = function() end end -- read-only

Thermo.X[0] = function(o) -- Start
  if o._f[T_STATE] == 1 and o._started then return end
  o._f[T_STATE], o._started = 1, true
  o:_fire(1)
  thermo_control(o)
end
Thermo.X[1] = function(o) -- Stop: output off
  if o._f[T_STATE] == 0 then return end
  o._f[T_STATE] = 0
  thermo_output(o, 0)
  o:_fire(2)
end
Thermo.X[2] = function(o) Thermo.S[T_POINT](o, (tonumber(o._f[T_POINT]) or 20) + 1) end
Thermo.X[3] = function(o) Thermo.S[T_POINT](o, (tonumber(o._f[T_POINT]) or 20) - 1) end
Thermo.X[4] = function(o, value, minutes) -- HeatUp(Value 0..15 °C, Time min)
  value, minutes = tonumber(value) or 0, tonumber(minutes) or 0
  if minutes <= 0 then return end
  local back = o._f[T_MODE] == 3 and o._heatup and o._heatup.back or o._f[T_MODE]
  o._heatup = { add = math.max(0, math.min(15, value)), till = C.millis() + minutes * 60000, back = back }
  Thermo.S[T_MODE](o, 3)
end
Thermo.X[5] = function(o) Thermo.S[T_MODE](o, 1) end
Thermo.X[6] = function(o) if o._f[T_MODE] == 1 then Thermo.S[T_MODE](o, 0) end end
Thermo.X[7] = function(o) Thermo.S[T_MODE](o, 2) end
Thermo.X[8] = function(o) if o._f[T_MODE] == 2 then Thermo.S[T_MODE](o, 0) end end

function Thermo.tick(o, now)
  if o._f[T_MODE] == 3 and o._heatup and now >= o._heatup.till then
    local back = o._heatup.back
    o._heatup = nil
    Thermo.S[T_MODE](o, back)
  end
  if now < o._next_read then return end
  local async = thermo_read(o)
  o._next_read = now + (async and 10000 or 1000)
  if not async then thermo_control(o) end
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

local function days_from_civil(y, m, d) -- days since 1970-01-01 (proleptic Gregorian)
  y = m <= 2 and y - 1 or y
  local era = (y >= 0 and y or y - 399) // 400
  local yoe = y - era * 400
  local doy = (153 * (m + (m > 2 and -3 or 9)) + 2) // 5 + d - 1
  local doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
  return era * 146097 + doe - 719468
end

-- UTC epoch of sunrise (rising) / sunset on calendar day y-m-d at lat/lon; nil when the sun does
-- not rise / set that day.
local function sun_event(y, m, d, lat, lon, rising)
  local n = days_from_civil(y, m, d) - days_from_civil(y, 1, 1) + 1
  local lng_hour = lon / 15
  local t = n + ((rising and 6 or 18) - lng_hour) / 24
  local M = 0.9856 * t - 3.289
  local L = (M + 1.916 * math.sin(M * rad) + 0.020 * math.sin(2 * M * rad) + 282.634) % 360
  local RA = (math.atan(0.91764 * math.tan(L * rad)) * deg) % 360
  RA = (RA + (L // 90) * 90 - (RA // 90) * 90) / 15
  local sin_dec = 0.39782 * math.sin(L * rad)
  local cos_dec = math.cos(math.asin(sin_dec))
  local cos_h = (math.cos(90.833 * rad) - sin_dec * math.sin(lat * rad)) / (cos_dec * math.cos(lat * rad))
  if cos_h > 1 or cos_h < -1 or cos_h ~= cos_h then return nil end
  local H = math.acos(cos_h) * deg
  if rising then H = 360 - H end
  local T = H / 15 + RA - 0.06571 * t - 6.622
  local ut = (T - lng_hour) % 24
  -- The event belongs to the local solar day y-m-d: its UTC hour is near 6/18 h - lon/15, which can
  -- fall on the previous/next UTC day (east: Sydney sunrise ~21 UTC the day before).
  local expect = (rising and 6 or 18) - lng_hour
  if ut - expect > 12 then ut = ut - 24 elseif expect - ut > 12 then ut = ut + 24 end
  return math.floor(days_from_civil(y, m, d) * 86400 + ut * 3600 + 0.5)
end
__sun_event = sun_event

local function utc_offset(ts)
  local z = os.date("%z", ts)
  local sign, h, mi = tostring(z):match("^([+-])(%d%d)(%d%d)$")
  if not sign then return 0 end
  local off = tonumber(h) * 3600 + tonumber(mi) * 60
  return sign == "-" and -off or off
end

local function sun_day(o, ts) -- sunrise/sunset for the local calendar day of ts (cached)
  local t = os.date("*t", ts)
  local lat, lon = tonumber(o._f[1]) or 0, tonumber(o._f[0]) or 0
  local key = string.format("%d-%d-%d/%s/%s", t.year, t.month, t.day, lat, lon)
  o._days = o._days or {}
  local c = o._days[key]
  if not c then
    if lat < -90 or lat > 90 or lon < -180 or lon > 180 then
      c = {}
    else
      c = { rise = sun_event(t.year, t.month, t.day, lat, lon, true),
            set = sun_event(t.year, t.month, t.day, lat, lon, false) }
    end
    o._ndays = (o._ndays or 0) + 1
    if o._ndays > 8 then o._days, o._ndays = {}, 1 end -- yesterday .. +2 days
    o._days[key] = c
  end
  return c
end

local function sun_next(o, ts, which, offset_idx)
  local off = (tonumber(o._f[offset_idx]) or 0) * 60
  for dd = -1, 2 do -- -1: CLU clock zone far from the location (its sunset after local midnight)
    local e = sun_day(o, ts + dd * 86400)[which]
    if e and e + off > ts then return e + off end
  end
  return nil
end

local function hhmm(ts, utc) return ts and os.date(utc and "!%H:%M" or "%H:%M", ts) or "N\\A" end

local function sun_update(o, now)
  local f, today = o._f, sun_day(o, now)
  f[3], f[4] = hhmm(today.rise, true), hhmm(today.set, true)
  f[5], f[6] = hhmm(today.rise), hhmm(today.set)
  f[7], f[8] = today.rise or -1, today.set or -1
  f[9] = today.rise and today.rise + utc_offset(today.rise) or -1
  f[10] = today.set and today.set + utc_offset(today.set) or -1
  if today.rise and today.set then
    f[11] = 0
    for dd = -1, 1 do
      local d = sun_day(o, now + dd * 86400)
      if d.rise and d.set and now >= d.rise and now < d.set then f[11] = 1 end
    end
  else
    f[11] = -1
  end
  local nr, ns = sun_next(o, now, "rise", 12), sun_next(o, now, "set", 13)
  f[14] = nr and math.ceil((nr - now) / 60) or -1
  f[15] = ns and math.ceil((ns - now) / 60) or -1
end

function Sun.init(o)
  o._f[0], o._f[1], o._f[2], o._f[12], o._f[13] = 0, 0, 1, 0, 0
  o._dirty = true
end
for i = 3, 11 do Sun.S[i] = function() end end -- read-only
Sun.S[14], Sun.S[15] = function() end, function() end
for _, i in ipairs({ 0, 1, 12, 13 }) do
  Sun.S[i] = function(o, v) o._f[i] = tonumber(v) or 0; o._dirty = true end
end
Sun.S[2] = function(o, v) if (tonumber(v) or 0) ~= 0 then Sun.X[0](o) else Sun.X[1](o) end end
Sun.X[0] = function(o)
  if o._f[2] == 1 and o._started then return end
  o._f[2], o._started, o._last = 1, true, nil
  o:_fire(0)
end
Sun.X[1] = function(o)
  if o._f[2] == 0 then return end
  o._f[2] = 0
  o:_fire(1)
end
for i = 3, 15 do
  Sun.G[i] = function(o)
    if o._dirty then o._dirty = false; sun_update(o, now_s()) end
    return o._f[i]
  end
end

function Sun.tick(o)
  local now = now_s()
  if o._last and now // 60 == o._last // 60 and not o._dirty then return end
  local prev, prev_day = o._last, o._f[11]
  sun_update(o, now)
  o._dirty, o._last = false, now
  if o._f[2] ~= 1 or not prev then return end
  -- events whose (offset) time passed since the previous check
  for _, e in ipairs({ { "rise", 12, 2 }, { "set", 13, 3 } }) do
    local at = sun_next(o, prev, e[1], e[2])
    if at and at <= now then
      o:_fire(e[3])
      o:_fire(4)
    end
  end
  if prev_day ~= o._f[11] and prev_day ~= -1 and o._f[11] ~= -1 then
    o:_fire(o._f[11] == 1 and 5 or 6)
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

local function parse_result(r)
  if r == nil then return "error" end
  if r == "nil" then return nil elseif r == "true" then return true elseif r == "false" then return false end
  local n = tonumber(r)
  if n ~= nil then return n end
  return r
end

-- Returns: sent, result (result only when the caller could wait for it).
local function remote(o, script)
  local id = C.remote(o._ip, script)
  if not id then return false, "error" end
  local waited, r = __wait(id)
  if not waited then return true, nil end
  return true, parse_result(r)
end

-- Lua source literal for a proxied argument. lit() (clientReport format) only escapes '"', so
-- backslashes, newlines or NULs in strings would break or alter the remote script; ser() is %q-based.
local function rlit(v) return ser(v) or "nil" end

function Remote:set(idx, v)
  self._f[idx] = v
  C.remote(self._ip, self._name .. ":set(" .. tostring(idx) .. ", " .. rlit(v) .. ")")
end

function Remote:get(idx)
  if not coroutine.isyieldable() then return self._f[idx] end
  local sent, r = remote(self, self._name .. ":get(" .. tostring(idx) .. ")")
  if sent and r ~= "error" then self._f[idx] = r end
  return r
end

function Remote:execute(idx, ...)
  if idx == 0 then
    local script = ...
    local _, r = remote(self, tostring(script))
    return r
  end
  local args = { tostring(idx) }
  for i = 1, select("#", ...) do args[#args + 1] = rlit((select(i, ...))) end
  local _, r = remote(self, self._name .. ":execute(" .. table.concat(args, ", ") .. ")")
  return r
end
