-- Host test for components/clu_lua/prelude.lua with a Lua stub of the C __clu table.
-- Usage: lua5.3 test_prelude.lua [om.lua path]   (default: fixtures/om.lua)
local here = arg[0]:match("(.*/)") or "./"
local om_path = arg[1] or (here .. "fixtures/om.lua")

local print = print -- prelude replaces the global print
local failures = 0
local function check(name, cond)
  print((cond and "ok   " or "FAIL ") .. name)
  if not cond then failures = failures + 1 end
end

-- ---- stub of the C side ------------------------------------------------------
local created, executed, reports, logs, remotes = {}, {}, {}, {}, {}
local remote_down = false
local clock = 0 -- fake __clu.millis()
-- persistent store in a temp file, pre-seeded as if written before a reboot
local storage = os.tmpname()
local storage_tmp = storage:gsub("%.%w+$", "") .. ".TMP"
do
  local f = assert(io.open(storage, "wb"))
  f:write('{["es"]={["EVE0002"]={{3,0,"0 9 * * *"}}},["vars"]={["keep"]="a\\"b\\nc",["restored"]=7}}')
  f:close()
end
local function stored()
  local f = io.open(storage, "rb")
  if not f then return nil end
  local t = f:read("a"); f:close()
  return load("return " .. t, "=s", "t", {})()
end
local hooks = {
  [0] = { get = function(o, idx) if idx == 0 then return true, 42 end end },
  [4] = { execute = function(o, idx) executed[#executed + 1] = o._name .. ":" .. idx; return 1 end },
}
__clu = {
  root = here .. "fixtures",
  storage = storage,
  new = function(o) created[#created + 1] = o end,
  get = function(o, idx)
    local h = hooks[o._class]
    if h and h.get then return h.get(o, idx) end
    return false
  end,
  set = function() return false end,
  execute = function(o, idx, ...)
    local h = hooks[o._class]
    if h and h.execute then return h.execute(o, idx, ...) end
  end,
  log = function(level, ...) logs[#logs + 1] = table.concat({ ... }, "\t") end,
  loop = function() if loop_err then error(loop_err) end end,
  init = function() end,
  wait = function() end,
  report = function(addr, port, body)
    if math.type(port) ~= "integer" then error("bad argument #2 to 'report' (number expected)") end
    reports[#reports + 1] = { addr, port, body }
  end,
  millis = function() return clock end,
  remote = function(ip, script)
    if remote_down then return nil end -- transport queue full
    remotes[#remotes + 1] = { ip, script }
    return #remotes -- call id
  end,
}

-- bytecode captured before the prelude removes string.dump (sandbox tests below)
local bytecode = string.dump(function() return 42 end)
local binmod = here .. "fixtures/binmod.lua"
do local f = assert(io.open(binmod, "wb")); f:write(bytecode); f:close() end
assert(loadfile(here .. "../../components/clu_lua/prelude.lua"))()
assert(loadfile(here .. "../../components/clu_lua/objects.lua"))()
__clu = nil -- done by clu_lua.c after both chunks
check("package.path uses drive root", package.path:find("fixtures/%?%.lua") ~= nil)

-- ---- MAIN.LUA equivalent, without the infinite loop ----------------------------
require "user"
assert(loadfile(om_path))()
function checkAlive() return "05f5e101" end
SYSTEM.Init()

local n_mod, n_io, n_clu = 0, 0, 0
for _, o in ipairs(created) do
  if o._class == 2 then n_mod = n_mod + 1
  elseif o._module then n_io = n_io + 1
  elseif o._ip then n_clu = n_clu + 1 end
end
print(string.format("     objects=%d modules=%d io=%d clu=%d", #created, n_mod, n_io, n_clu))
check("objects created", #created > 0)
check("modules parsed", n_mod > 0)
check("io objects bound to module tables", n_io > 0)
check("local CLU found", __local_clu() ~= nil and __local_clu()._class == 0)
check("OnInit fired", arg[1] ~= nil or initDone == true)
check("require user", userFn() == "user-ok")

if not arg[1] then
  check("module sn/hwType", mm_330000001._sn == 0x13ab6c81 and mm_330000001._hwType == 0x1e)
  check("io module/channel", DOU0002._module == mm_330000001 and DOU0002._channel == 1)
  check("set stores value", ROL0001:get(1) == 30000)
  check("class hook get", CLU100000001:get(0) == 42)
  check("generic get on CLU", CLU100000001:get(22) == "pool.ntp.org")
  check("execute via hook", DOU0001:execute(0) == 1 and executed[#executed] == "DOU0001:0")

  __fire("DIN0001", 0)
  check("__fire runs add_event handler", executed[#executed] == "DOU0001:0" and #executed == 2)
  __update("DIN0001", 0, 1)
  check("__update stores feature", DIN0001:get(0) == 1)

  -- push subscriptions
  __req_from = "10.0.0.9"
  local r = SYSTEM:clientRegister("10.0.0.5", 4344, 7, { { DIN0001, 0 }, { ROL0001, 1 }, { STO99999, 3 } })
  check("clientRegister snapshot", r == "clientReport:7:{1,30000,nil}")
  SYSTEM.Loop()
  check("no report without change", #reports == 0)
  __update("DIN0001", 0, 0)
  SYSTEM.Loop()
  check("report on change to addr and source",
    #reports == 2 and reports[1][1] == "10.0.0.5" and reports[2][1] == "10.0.0.9" and
    reports[1][2] == 4344 and reports[1][3] == "clientReport:7:{0,30000,nil}")
  check("fetchValues", SYSTEM:fetchValues({ { DOU0001, 0 }, { "ROL0001", 1 } }) == "values:{0,30000}")
  check("clientDestroy", SYSTEM:clientDestroy("10.0.0.5", 4344, 7) == 7)
  __update("DIN0001", 0, 1)
  SYSTEM.Loop()
  check("no report after destroy", #reports == 2)
  for i = 1, 20 do SYSTEM:clientRegister("10.0.1." .. i, 4344, i, { { DIN0001, 0 } }) end
  reports = {}
  __update("DIN0001", 0, 0)
  SYSTEM.Loop()
  check("client registry bounded to 16 (oldest dropped)",
    #reports == 32 and reports[1][1] ~= "10.0.1.1" and reports[1][1] ~= "10.0.1.4")
  for i = 1, 20 do SYSTEM:clientDestroy("10.0.1." .. i, 4344, i) end
  reports = {}

  -- errors inside SYSTEM.Loop must not escape (MAIN.LUA would end -> emergency mode)
  SYSTEM:clientRegister("10.0.2.1", "bad", 1, { { DIN0001, 0 } })
  SYSTEM:clientRegister("10.0.2.2", 4344, 2, { { DIN0001, 0 } })
  __update("DIN0001", 0, 1)
  local lok = pcall(SYSTEM.Loop)
  check("failing clientReport does not escape SYSTEM.Loop", lok)
  check("other clients still reported", #reports == 2 and reports[1][1] == "10.0.2.2")
  check("failing client dropped", SYSTEM:clientDestroy("10.0.2.1", "bad", 1) == 1 and
    logs[#logs]:find("dropped") ~= nil)
  loop_err = "boom"
  lok = pcall(SYSTEM.Loop)
  loop_err = nil
  check("C.loop error does not escape SYSTEM.Loop", lok and logs[#logs]:find("boom") ~= nil)
  SYSTEM:clientDestroy("10.0.2.2", 4344, 2)
  reports = {}

  -- OnTimeChange (event 13 on local CLU) when wall clock jumps vs monotonic clock
  local tch = 0
  CLU100000001:add_event(13, function() tch = tch + 1 end)
  local rt = os.time
  local base = rt()
  os.time = function(t) if t then return rt(t) end return base end
  clock = clock + 2000; SYSTEM.Loop()
  base = base + 2; clock = clock + 2000; SYSTEM.Loop()
  check("no OnTimeChange on normal tick", tch == 0)
  base = base + 3600; clock = clock + 2000; SYSTEM.Loop()
  check("OnTimeChange on clock jump", tch == 1)
  os.time = rt
  check("string literal escaping", SYSTEM:fetchValues({ { CLU100000001, 22 } }) == 'values:{"pool.ntp.org"}')
end

-- ---- virtual objects ----------------------------------------------------------------------
do
  local fired = {}
  local function on(o, ...)
    for _, ev in ipairs({ ... }) do
      o:add_event(ev, function() fired[#fired + 1] = o._name .. ":" .. ev end)
    end
  end
  local function took() local f = table.concat(fired, " "); fired = {}; return f end
  local function advance(ms) clock = clock + ms; SYSTEM.Loop() end

  -- Timer
  local T = OBJECT:new(6, "TIM0001")
  on(T, 0, 1, 2, 3)
  check("timer defaults", T:get(0) == 1000 and T:get(1) == 0 and T:get(2) == 0)
  T:set(0, 500)
  T:execute(0)
  check("timer start", T:get(2) == 1 and took() == "TIM0001:1")
  advance(200)
  check("timer value counts down", T:get(3) == 300)
  T:execute(2)
  advance(1000)
  check("timer pause holds value", T:get(2) == 2 and T:get(3) == 300 and took() == "TIM0001:3")
  T:execute(2)
  advance(299)
  check("timer resumed, not yet expired", T:get(2) == 1 and took() == "")
  advance(1)
  check("countdown stops (OnStop) then fires OnTimer", T:get(2) == 0 and took() == "TIM0001:2 TIM0001:0")
  T:set(1, 1)
  T:execute(0); took()
  advance(1100)
  check("interval fires and keeps phase", took() == "TIM0001:0" and T:get(2) == 1 and T:get(3) == 400)
  T:execute(1)
  check("timer stop", T:get(2) == 0 and took() == "TIM0001:2")

  -- cron
  local cron = __cron
  local function ts(y, m, d, h, mi) return os.time({ year = y, month = m, day = d, hour = h, min = mi, sec = 0 }) end
  local r = cron.parse("30 7 * * 1-5")
  check("cron parse", r ~= nil and cron.parse("61 * * * *") == nil and cron.parse("* * *") == nil)
  check("cron match weekday", cron.match(r, ts(2026, 11, 9, 7, 30)) and not cron.match(r, ts(2026, 11, 8, 7, 30)))
  check("cron next skips weekend", cron.next(r, ts(2026, 11, 7, 12, 0)) == ts(2026, 11, 9, 7, 30))
  check("cron step/list", cron.next(cron.parse("*/15 8,20 * * *"), ts(2026, 1, 1, 8, 50)) == ts(2026, 1, 1, 20, 0))
  check("cron dom|dow (OR)", cron.next(cron.parse("0 0 13 * 5"), ts(2026, 2, 1, 0, 0)) == ts(2026, 2, 6, 0, 0))
  check("cron sunday as 7", cron.match(cron.parse("0 12 * * 7"), ts(2026, 11, 8, 12, 0)))
  check("cron impossible date", cron.next(cron.parse("0 0 31 2 *"), ts(2026, 1, 1, 0, 0)) == nil)

  -- Calendar (fake wall clock)
  local real_time = os.time
  local wall = ts(2026, 11, 9, 7, 28)
  os.time = function(t) if t then return real_time(t) end return wall end
  local CAL = OBJECT:new(15, "CAL0001")
  on(CAL, 0, 3)
  CAL:set(0, "30 7 * * *")
  check("calendar rule", CAL:get(0) == "30 7 * * *" and CAL:get(3) == 1)
  SYSTEM.Loop()
  check("calendar ToNextRun", CAL:get(2) == 2)
  wall = wall + 120; SYSTEM.Loop()
  check("calendar fires on match", took() == "CAL0001:0" and CAL:get(1) == 0)
  SYSTEM.Loop()
  check("calendar fires once per minute", took() == "")
  CAL:execute(3, 1); took()
  wall = wall + 86400; SYSTEM.Loop()
  check("calendar CancelNext skips one run", took() == "")
  wall = wall + 86400; SYSTEM.Loop()
  check("calendar runs again", took() == "CAL0001:0")
  CAL:set(0, "bad rule")
  check("calendar bad rule", CAL:get(0) == "ERROR")

  -- Storage (class 44) + persistence
  check("storage restored before OnInit", restored == 7 and keep == 'a"b\nc')
  check("ser int/float/str", __ser(1) == "1" and __ser(1.0) == "1.0" and __ser(0.1) == "0.10000000000000001"
    and __ser("x\0y") == '"x\\0y"')
  myvar = 5
  STO99999:execute(4, "myvar")
  check("store writes now", stored().vars.myvar == 5 and math.type(stored().vars.myvar) == "integer")
  myvar = 2.5
  clock = clock + 1000; SYSTEM.Loop()
  check("tracked change debounced", stored().vars.myvar == 5)
  clock = clock + 5000; SYSTEM.Loop()
  check("tracked change written", stored().vars.myvar == 2.5 and stored().vars.restored == 7)
  -- reset right after a change (no SYSTEM.Loop scan / debounce yet): __shutdown writes it
  myvar = 3
  __shutdown()
  check("close hook: unscanned change written before the VM closes", stored().vars.myvar == 3)
  myvar = 2.5
  clock = clock + 1000; SYSTEM.Loop(); clock = clock + 5000; SYSTEM.Loop()
  check("utilization %", STO99999:get(1) == 0)
  local ES2 = OBJECT:new(43, "EVE0002")
  check("es rules restored", ES2:get(0) == '{{3,0,"0 9 * * *"}}')
  check("es add duplicate -> same id", ES2:execute(2, "0 9 * * *") == 3 and ES2:get(3) == 1)

  -- EventScheduler
  local ES = OBJECT:new(43, "EVE0001")
  on(ES, 2, 3, 4)
  check("es add", ES:execute(2, "45 7 * * *") == 1 and ES:execute(2, "0 8 * * *") == 2 and ES:execute(2, "x") == 0)
  check("es list", ES:get(0) == '{{1,1,"45 7 * * *"},{2,1,"0 8 * * *"}}' and ES:get(3) == 2 and ES:get(4) == 14)
  check("es disable", ES:execute(6, 1) == 0 and ES:execute(4, 1) == '{1,0,"45 7 * * *"}' and ES:execute(6, 9) == 1)
  check("es next rule", ES:get(2) == '{2,1,"0 8 * * *"}')
  took()
  wall = ts(2026, 11, 10, 7, 45); SYSTEM.Loop()
  check("es disabled rule silent", took() == "")
  wall = ts(2026, 11, 10, 8, 0); SYSTEM.Loop()
  check("es fires OnEvent", took() == "EVE0001:2" and ES:get(1) == '{2,1,"0 8 * * *"}')
  check("es delete", ES:execute(3, 1) == 0 and ES:get(3) == 1 and took() == "EVE0001:4")
  clock = clock + 6000; SYSTEM.Loop()
  local es_saved = stored().es.EVE0001
  check("es rules persisted", #es_saved == 1 and es_saved[1][1] == 2 and es_saved[1][3] == "0 8 * * *")

  -- power cut between remove and rename: .TMP is used
  os.rename(storage, storage_tmp)
  __persist.data = nil
  check("load falls back to .TMP", __persist.load().vars.myvar == 2.5)
  STO99999:execute(3)
  check("erase all", next(stored().vars) == nil and stored().es.EVE0001 ~= nil and io.open(storage_tmp) == nil)
  os.time = real_time

  -- Statistics / StatisticsPulse (CLU interface 0x203)
  local ST = OBJECT:new(47, "STA0001")
  on(ST, 0)
  check("statistics defaults", ST:get(0) == 0 and ST:get(4) == -1 and ST:get(6) == 0)
  ST:set(0, 12.5); ST:set(0, 12.5)
  check("statistics SetValue: OnValueChange once", ST:get(0) == 12.5 and took() == "STA0001:0")
  local SP = OBJECT:new(48, "STP0001")
  on(SP, 1)
  SP:execute(0); SP:execute(0)
  check("statistics pulse: TriggerPulse counts + OnPulseTriggered",
        SP:get(10) == 2 and took() == "STP0001:1 STP0001:1")

  -- PresenceSensor
  local PS = OBJECT:new(40, "PRE0001")
  on(PS, 2, 3)
  PS:set(0, 10)
  PS:execute(2)
  check("presence on", PS:get(2) == 1 and took() == "PRE0001:2")
  advance(9000); PS:execute(2); advance(9000)
  check("presence retriggered", PS:get(2) == 1 and PS:get(3) == 9 and took() == "")
  advance(1000)
  check("presence timeout off", PS:get(2) == 0 and took() == "PRE0001:3")
  PS:execute(4)
  check("presence lock on", PS:get(5) == 1 and PS:get(2) == 1 and took() == "PRE0001:2")
  advance(60000)
  check("presence locked holds", PS:get(2) == 1)
  PS:execute(4)
  check("presence unlock off", PS:get(5) == 0 and PS:get(2) == 0 and took() == "PRE0001:3")
  PS:execute(2)
  check("presence ignored within DetectionDelay", PS:get(2) == 0)
  advance(2000); PS:execute(2)
  check("presence after DetectionDelay", PS:get(2) == 1 and took() == "PRE0001:2")

  -- Push
  local PU = OBJECT:new(31, "PUS0001")
  on(PU, 0)
  PU:set(1, "Alarm"); PU:set(0, "door "); PU:set(0, "open")
  PU:execute(4)
  check("push send", took() == "PUS0001:0" and PU:get(0) == "door open" and PU:get(2) ~= "")
  PU:execute(4)
  check("push interval limit", took() == "")

  -- Remote CLU
  local R = OBJECT:new(1, 0xC0A80191, "CLU221010198")
  R:execute(0, 'DIM1186:execute(1, 0, 2000)')
  R:set(3, "x\"y")
  R:execute(5, 1, "a", true)
  check("remote execute(0) sends script", remotes[1][1] == 0xC0A80191 and remotes[1][2] == 'DIM1186:execute(1, 0, 2000)')
  check("remote set proxies", remotes[2][2] == 'CLU221010198:set(3, "x\\"y")' and R:get(3) == 'x"y')
  check("remote execute(n) proxies", remotes[3][2] == 'CLU221010198:execute(5, 1, "a", true)')
  check("remote calls outside a handler return nil at once", R:execute(0, "x") == nil and R:get(3) == 'x"y')

  -- Inside an event handler the call yields until the reply (as on the real CLU)
  local B = OBJECT:new(6, "TIMR001")
  local got = {}
  B:add_event(1, function()
    got[#got + 1] = R:execute(0, "DOU1:get(0)")
    got[#got + 1] = R:get(7)
    got[#got + 1] = "done"
  end)
  B:execute(0) -- OnStart -> handler runs until the first remote call
  local id1 = #remotes
  check("handler parked on remote call", remotes[id1][2] == "DOU1:get(0)" and #got == 0)
  __remote_done(id1, "1")
  local id2 = #remotes
  check("resumed with number result, next call parked", got[1] == 1 and math.type(got[1]) == "integer"
    and remotes[id2][2] == "CLU221010198:get(7)" and #got == 1)
  __remote_done(id2, "hello")
  check("get returns remote string, cached", got[2] == "hello" and got[3] == "done" and R._f[7] == "hello")
  __remote_done(id2, "again") -- duplicate / unknown id: ignored
  check("unknown reply id ignored", #got == 3)

  -- result parsing, timeout -> "error", transport down -> "error"
  local res = {}
  local B2 = OBJECT:new(6, "TIMR002")
  B2:add_event(1, function()
    for _ = 1, 5 do res[#res + 1] = tostring(R:execute(0, "v")) end
    remote_down = true
    res[#res + 1] = R:execute(0, "v")
    remote_down = false
  end)
  B2:execute(0)
  __remote_done(#remotes, "true")
  __remote_done(#remotes, "false")
  __remote_done(#remotes, "nil")
  __remote_done(#remotes, "0.5")
  __remote_done(#remotes, nil) -- C: timeout / send failure
  check("result parsing + timeout + not sent",
    table.concat(res, ",") == "true,false,nil,0.5,error,error")

  -- lost reply: the safety timeout resumes the thread with "error"
  local lost
  B2._ev[1] = { function() lost = R:execute(0, "w") end }
  B2:execute(0)
  clock = clock + 10001; SYSTEM.Loop()
  check("lost reply -> error after 10 s", lost == "error")

  -- error after resuming is logged with the handler's name
  local nlogs = #logs
  B2._ev[1] = { function() R:execute(0, "z"); error("late boom") end }
  B2:execute(0)
  __remote_done(#remotes, "1")
  check("error after resume logged with handler name",
    #logs == nlogs + 1 and logs[#logs]:find("TIMR002 event 1") and logs[#logs]:find("late boom"))
end

-- ---- more virtual-object / prelude edge cases ---------------------------------------------
do
  local fired = {}
  local function on(o, ...)
    for _, ev in ipairs({ ... }) do
      o:add_event(ev, function() fired[#fired + 1] = o._name .. ":" .. ev end)
    end
  end
  local function took() local f = table.concat(fired, " "); fired = {}; return f end
  local function advance(ms) clock = clock + ms; SYSTEM.Loop() end
  local real_time = os.time
  local function ts(y, m, d, h, mi) return real_time({ year = y, month = m, day = d, hour = h, min = mi, sec = 0 }) end
  local wall = ts(2026, 11, 9, 6, 0)
  os.time = function(t) if t then return real_time(t) end return wall end

  -- event handler error: logged, later handlers still run
  local EH = OBJECT:new(6, "TIM0009")
  EH:add_event(1, function() error("boom") end)
  on(EH, 1)
  local nlogs = #logs
  EH:execute(0)
  check("failing handler logged, next handler runs",
    took() == "TIM0009:1" and #logs == nlogs + 1 and logs[#logs]:find("TIM0009 event 1") ~= nil)
  EH:execute(1)

  -- Timer: Time changed while running restarts the countdown with the new time
  local T = OBJECT:new(6, "TIM0010")
  on(T, 0)
  T:set(0, 1000); T:execute(0)
  advance(800)
  T:set(0, 500)
  advance(400)
  check("timer Time set while running restarts", T:get(3) == 100 and took() == "")
  advance(100)
  check("timer fires after new Time", took() == "TIM0010:0")

  -- same name declared again (e.g. via req:): the old object stops ticking, no duplicates kept
  local OLD = OBJECT:new(6, "TIM0011")
  on(OLD, 0)
  OLD:set(0, 100); OLD:execute(0)
  local NEW = OBJECT:new(6, "TIM0011")
  advance(200)
  check("redeclared timer: old instance no longer ticks", took() == "")
  local nmod = #__modules
  OBJECT:new(2, 0x7abcdef0, 0x1e, "mm_dup")
  OBJECT:new(2, 0x7abcdef0, 0x1e, "mm_dup")
  OBJECT:new(2, 0x7abcdef0, 0x1e, "mm_dup2")
  check("redeclared module (same name / same SN) kept once", #__modules == nmod + 1 and
    __modules[#__modules]._name == "mm_dup2")

  -- Calendar Stop/Start and SinceLastRun
  local CAL = OBJECT:new(15, "CAL0009")
  on(CAL, 0, 1, 2)
  CAL:set(0, "* * * * *")
  SYSTEM.Loop() -- first minute seen: no fire (minute boundary not crossed yet)
  wall = wall + 60; SYSTEM.Loop()
  check("calendar every minute fires", took() == "CAL0009:0" and CAL:get(1) == 0)
  wall = wall + 120
  check("calendar SinceLastRun in minutes", CAL:get(1) == 2)
  CAL:execute(1)
  wall = wall + 60; SYSTEM.Loop()
  check("calendar stopped: OnStop, no OnCalendar, ToNextRun 0",
    took() == "CAL0009:2" and CAL:get(3) == 0 and CAL:get(2) == 0)
  CAL:execute(0)
  wall = wall + 60; SYSTEM.Loop()
  check("calendar restarted fires again", took() == "CAL0009:1 CAL0009:0")
  CAL:execute(1); took()

  -- EventScheduler: capacity, disabled scheduler, clear all
  local ES = OBJECT:new(43, "EVE0009")
  on(ES, 1, 2, 4)
  for i = 1, 16 do ES:execute(2, i .. " 7 * * *") end
  check("es full: 17th AddRule -> 0, FreeSlots 0", ES:execute(2, "0 23 * * *") == 0 and ES:get(4) == 0)
  ES:execute(9); ES:execute(9)
  check("es clear all: one OnRuleDelete, list empty", took() == "EVE0009:4" and ES:get(3) == 0 and ES:get(0) == "{}")
  ES:execute(2, "* * * * *")
  ES:execute(1)
  SYSTEM.Loop(); wall = wall + 60; SYSTEM.Loop()
  check("es stopped scheduler: OnStop, no OnEvent", took() == "EVE0009:1")
  check("es unknown rule id", ES:execute(4, 99) == "N\\A" and ES:execute(3, 99) == 1 and ES:execute(5, 99) == 1)
  ES:execute(9); took()
  os.time = real_time

  -- PresenceSensor: Mode 1 (state) holds while present; State off ignores detection
  local PS = OBJECT:new(40, "PRE0009")
  on(PS, 2, 3)
  PS:set(0, 5); PS:set(6, 1)
  PS:execute(2)
  advance(60000)
  check("presence mode 1: no timeout while held", PS:get(2) == 1 and PS:get(3) == 0 and took() == "PRE0009:2")
  PS:execute(3)
  advance(4000)
  check("presence mode 1: counts after Undetect", PS:get(2) == 1 and PS:get(3) == 4)
  advance(1000)
  check("presence mode 1: off after Timeout", PS:get(2) == 0 and took() == "PRE0009:3")
  PS:execute(1)
  advance(3000)
  PS:execute(2)
  check("presence State 0: detection ignored", PS:get(2) == 0 and took() == "")

  -- Push: message longer than 500 chars truncated, OnOverflow fired
  local PU = OBJECT:new(31, "PUS0009")
  on(PU, 1)
  PU:set(0, string.rep("x", 499)); PU:set(0, "yz")
  check("push overflow", #PU:get(0) == 500 and took() == "PUS0009:1")
  PU:execute(1)
  check("push clear message", PU:get(0) == "")

  -- Scheduler (class 16): state machine only
  local SC = OBJECT:new(16, "SCH0009")
  on(SC, 2)
  SC:execute(1); SC:set(1, 1)
  check("scheduler stop + read-only State", SC:get(1) == 0 and took() == "SCH0009:2")

  -- unknown method on a virtual class: logged, returns nil
  nlogs = #logs
  check("unknown execute logged", PU:execute(42) == nil and #logs == nlogs + 1 and logs[#logs]:find("execute%(42%)"))

  -- fetchValues on an unknown object name / feature
  check("fetchValues unknown object -> nil", SYSTEM:fetchValues({ { "NOPE0001", 0 }, { PU, 99 } }) == "values:{nil,nil}")

  -- Remote CLU: strings with backslash / newline / quotes must reach the remote CLU unchanged
  local R = OBJECT:new(1, 0xC0A80192, "CLU221010199")
  local tricky = 'a\\b\n"c"\0'
  R:set(3, tricky)
  local got
  local env = { CLU221010199 = { set = function(_, i, v) got = { i, v } end } }
  local fn = load(remotes[#remotes][2], "=remote", "t", env)
  check("remote set script is valid Lua", fn ~= nil)
  if fn then fn() end
  check("remote set string round-trips", got and got[1] == 3 and got[2] == tricky)
  R:execute(7, tricky, 1.5, false)
  local args
  env.CLU221010199.execute = function(_, ...) args = { ... } end
  fn = load(remotes[#remotes][2], "=remote", "t", env)
  if fn then fn() end
  check("remote execute args round-trip", args and args[1] == 7 and args[2] == tricky and args[3] == 1.5 and args[4] == false)

  -- serializer round-trip (storage file)
  local v = { 1, 2.5, -0.0, 1 / 0, -1 / 0, "q\"\n\\", true, { n = { 3 } } }
  local back = load("return " .. __ser(v), "=s", "t", {})()
  check("ser round-trip", back[1] == 1 and math.type(back[1]) == "integer" and back[2] == 2.5 and
    back[4] == math.huge and back[5] == -math.huge and back[6] == v[6] and back[7] == true and back[8].n[1] == 3)
  local nan = load("return " .. __ser(0 / 0), "=s", "t", {})()
  check("ser NaN", nan ~= nan)
  check("ser rejects functions", __ser(print) == nil and __ser({ f = print }) == "{}")

  -- corrupted storage file: ignored with a log, store starts empty
  local f = assert(io.open(storage, "wb")); f:write("{broken"); f:close()
  __persist.data = nil
  nlogs = #logs
  local d = __persist.load()
  check("corrupted storage ignored", next(d) == nil and #logs == nlogs + 1 and logs[#logs]:find("unreadable"))
end

-- ---- sandbox: text-only code loading ---------------------------------------------------
do
  local fn, err = load(bytecode)
  check("load rejects bytecode", fn == nil and tostring(err):find("binary") ~= nil)
  check("load rejects bytecode even with mode 'b'", load(bytecode, "x", "b") == nil)
  check("load reader function with bytecode rejected",
    load(function() local b = bytecode; bytecode = nil; return b end) == nil)
  bytecode = string.char(27) .. "Lua"
  check("loadfile rejects bytecode", loadfile(binmod) == nil)
  check("dofile rejects bytecode", not pcall(dofile, binmod))
  check("require rejects bytecode module", not pcall(require, "binmod"))
  check("require still loads text modules", require("user") ~= nil and userFn() == "user-ok")
  check("load text still works", load("return 1 + 1")() == 2)
  check("load env argument kept", load("return x", "n", "t", { x = 5 })() == 5)
  check("load explicit nil env kept (no _ENV)", not pcall(load("return print", "n", "t", nil)) or
    load("return print", "n", "t", nil)() == nil)
  check("string.dump removed", string.dump == nil)
  check("debug reduced to traceback", type(debug.traceback) == "function" and debug.getlocal == nil and
    debug.setupvalue == nil and debug.upvaluejoin == nil and package.loaded.debug == debug)
  check("package.loadlib removed", package.loadlib == nil)
  check("deep nesting is a Lua error, not a crash",
    load("return " .. ("("):rep(300) .. "1" .. (")"):rep(300)) == nil)
end
os.remove(binmod)

-- ---- schedule Data, Scheduler, Thermostat, SunriseSunsetCalendar ---------------------------
do
  local real_time = os.time
  local function ts(y, m, d, h, mi) return real_time({ year = y, month = m, day = d, hour = h, min = mi, sec = 0 }) end
  local wall = ts(2026, 11, 9, 7, 2) -- Monday (wday 2 -> row 1), slot 28
  os.time = function(t) if t then return real_time(t) end return wall end
  local fired = {}
  local function on(o, ...)
    for _, ev in ipairs({ ... }) do
      o:add_event(ev, function() fired[#fired + 1] = o._name .. ":" .. ev end)
    end
  end
  local function took() local f = table.concat(fired, " "); fired = {}; return f end

  -- codec: OM default Data (all slots 5399 = "X~", suffix "+00:X~") = 20.0 on 0..30
  local default = string.rep("X~", 7 * 96) .. "+00:X~"
  local rows = __sched.decode(default)
  check("schedule decode OM default", rows and rows[0][0] == 5399 and rows[6][95] == 5399
    and math.abs(__sched.value(5399, 0, 30) - 20) < 0.01)
  check("schedule decode rejects short / bad chars", __sched.decode("Yz") == nil
    and __sched.decode(string.rep("\1\1", 7 * 96)) == nil)
  local r2 = {}
  for d = 0, 6 do r2[d] = {} for q = 0, 95 do r2[d][q] = (d * 96 + q) * 8 end end
  local back = __sched.decode(__sched.encode(r2))
  check("schedule encode/decode round-trip", back[1][28] == (96 + 28) * 8 and back[6][95] == (6 * 96 + 95) * 8)

  -- Scheduler: Value from the current quarter-hour, OnHarmonogram per slot
  local SC = OBJECT:new(16, "SCH0100")
  on(SC, 0, 1, 2)
  r2[1][28], r2[1][29] = 4050, 8099 -- Monday 07:00 -> 0.5, 07:15 -> 1.0
  SC:set(4, 1)
  SC:set(0, __sched.encode(r2))
  SYSTEM.Loop()
  check("scheduler value for current slot", SC:get(2) == 0.5 and took() == "SCH0100:0")
  wall = wall + 60; SYSTEM.Loop()
  check("scheduler: same slot, no event", took() == "")
  wall = ts(2026, 11, 9, 7, 15); SYSTEM.Loop()
  check("scheduler: next slot fires", SC:get(2) == 1 and took() == "SCH0100:0")
  SC:execute(1)
  wall = ts(2026, 11, 9, 7, 30); SYSTEM.Loop()
  check("scheduler stopped: no events", SC:get(1) == 0 and took() == "SCH0100:2")
  SC:execute(0); SYSTEM.Loop()
  check("scheduler start re-evaluates", took() == "SCH0100:1 SCH0100:0")
  SC:execute(1); took()

  -- Thermostat with a local sensor object and a local output object
  local SEN = OBJECT:new(98, "SEN0100") -- plain generic object: _f store
  local OUT = OBJECT:new(99, "DOU0100")
  SEN._f[0] = 19
  local TH = OBJECT:new(18, "THE0100")
  on(TH, 0, 1, 2, 3, 4, 5, 6)
  TH:set(0, { SEN, 0 })
  TH:set(1, { OUT, 0 })
  TH:set(3, 21) -- PointValue
  check("thermostat OnChange on PointValue", took() == "THE0100:0")
  clock = clock + 1000; SYSTEM.Loop()
  check("thermostat heats below target - hyst/2", OUT._f[0] == 1 and TH:get(13) == 1 and TH:get(14) == 19
    and TH:get(12) == 21 and took() == "THE0100:3")
  SEN._f[0] = 21.4; clock = clock + 1000; SYSTEM.Loop()
  check("inside hysteresis: stays on", OUT._f[0] == 1 and took() == "")
  SEN._f[0] = 21.5; clock = clock + 1000; SYSTEM.Loop()
  check("off at target + hyst/2", OUT._f[0] == 0 and took() == "THE0100:4")
  TH:execute(5) -- holiday 17
  clock = clock + 1000; SYSTEM.Loop()
  check("holiday mode target", TH:get(8) == 1 and TH:get(12) == 17 and took() == "THE0100:5")
  TH:execute(6)
  check("holiday off -> manual", TH:get(8) == 0 and took() == "THE0100:6")
  TH:execute(4, 3, 1) -- HeatUp +3 °C for 1 min
  clock = clock + 1000; SYSTEM.Loop()
  check("heat-up raises target", TH:get(8) == 3 and TH:get(12) == 24 and OUT._f[0] == 1 and took() == "THE0100:3")
  clock = clock + 60000; SYSTEM.Loop()
  check("heat-up ends, back to manual", TH:get(8) == 0 and TH:get(12) == 21 and OUT._f[0] == 0)
  took()
  TH:set(9, __sched.encode(r2)); TH:execute(7) -- AutoMode: schedule on Min..Max (0..30)
  wall = ts(2026, 11, 9, 7, 0); clock = clock + 1000; SYSTEM.Loop()
  check("auto mode target from schedule", TH:get(8) == 2 and TH:get(12) == 15)
  TH:set(7, 1) -- cooling: 21.5 > 15 + 0.5 -> on
  clock = clock + 1000; SYSTEM.Loop()
  check("reverse direction cools", OUT._f[0] == 1)
  TH:execute(1)
  check("stop forces output off", TH:get(6) == 0 and OUT._f[0] == 0 and took() == "THE0100:3 THE0100:4 THE0100:2")
  TH:execute(0)
  check("start", TH:get(6) == 1 and took():find("^THE0100:1") ~= nil)

  -- analog output, user-feature source, remote Control string with %v
  local TH2 = OBJECT:new(18, "THE0101")
  probe_temp = 20
  TH2:set(0, "probe_temp")
  TH2:set(2, 1)
  TH2:set(3, 20.25)
  local nrem = #remotes
  TH2:set(1, 'CLU221010198:execute(0, "DIM1:set(0,%v)")')
  CLU221010198 = OBJECT:new(1, 0xC0A80191, "CLU221010198")
  clock = clock + 1000; SYSTEM.Loop()
  check("analog output proportional over hysteresis", TH2:get(13) == 0.75
    and remotes[#remotes][2] == "DIM1:set(0,0.75)" and #remotes == nrem + 1)

  -- remote Source expression: read through a Lua thread, value arrives with the reply
  local TH3 = OBJECT:new(18, "THE0102")
  TH3:set(0, 'CLU221010198:execute(0, "SEN9:get(0)")')
  TH3:set(1, { OUT, 0 })
  clock = clock + 1000; SYSTEM.Loop()
  check("remote source requested", remotes[#remotes][2] == "SEN9:get(0)" and TH3:get(14) == nil)
  __remote_done(#remotes, "18.5")
  check("remote source value applied", TH3:get(14) == 18.5 and OUT._f[0] == 1)

  -- Sunrise/sunset: Warsaw 2026-06-21 (NOAA: sunrise 02:14 UTC, sunset 19:01 UTC, ±2 min)
  local rise = __sun_event(2026, 6, 21, 52.2297, 21.0122, true)
  local set = __sun_event(2026, 6, 21, 52.2297, 21.0122, false)
  local function near(t, hhmm)
    local h, m = hhmm:match("(%d+):(%d+)")
    local want = (real_time({ year = 2026, month = 6, day = 21, hour = 12 }) // 86400) * 86400 + h * 3600 + m * 60
    return t and math.abs(t - want) <= 180
  end
  check("sunrise/sunset Warsaw midsummer", near(rise, "02:14") and near(set, "19:01"))
  -- east longitude: Sydney's 21 June sunrise (07:00 AEST) is 20:59 UTC on 20 June
  local syd = __sun_event(2026, 6, 21, -33.87, 151.21, true)
  check("Sydney sunrise lands on the previous UTC day", syd and near(syd + 86400, "21:00"))
  check("polar night / polar day: nil", __sun_event(2026, 12, 21, 80, 15, true) == nil
    and __sun_event(2026, 6, 21, 80, 15, false) == nil)

  local SUN = OBJECT:new(39, "SUN0100")
  on(SUN, 2, 3, 4, 5, 6)
  SUN:set(1, 52.2297); SUN:set(0, 21.0122)
  wall = rise + 6 * 3600 -- daytime on 21 June in any test TZ from UTC-4 to UTC+10
  check("sun features", SUN:get(3) == os.date("!%H:%M", rise) and SUN:get(7) == rise and SUN:get(8) == set
    and SUN:get(11) == 1 and SUN:get(15) == math.ceil((set - wall) / 60))
  SYSTEM.Loop()
  wall = set - 30; SYSTEM.Loop()
  took()
  wall = set + 30; SYSTEM.Loop()
  check("sunset fires OnSunset + change + OnNight", took() == "SUN0100:3 SUN0100:4 SUN0100:6" and SUN:get(11) == 0)
  SUN:set(12, 10) -- sunrise offset +10 min
  wall = rise + 9 * 60 + 86400; SYSTEM.Loop() -- next day, close to its sunrise
  took()
  wall = wall + 120; SYSTEM.Loop()
  check("sunrise + offset fires OnSunrise", took():find("SUN0100:2 SUN0100:4") ~= nil)
  SUN:set(1, 80); SUN:set(0, 15)
  wall = ts(2026, 12, 21, 12, 0)
  check("polar night: N\\A and -1", SUN:get(3) == "N\\A" and SUN:get(7) == -1 and SUN:get(11) == -1
    and SUN:get(14) == -1)
  os.time = real_time
end

os.remove(storage); os.remove(storage_tmp)
for _, l in ipairs(logs) do print("     log: " .. l) end
print(failures == 0 and "\nPASSED (0 failures)" or ("\nFAILED (" .. failures .. " failures)"))
os.exit(failures == 0 and 0 or 1)
