-- Line coverage for the Lua tests: lua -e "COVOUT='out'" lua_cov.lua test.lua [args]
-- Appends "<source>\t<line>" for every line of components/ code that ran (see coverage.py).
local getinfo, sethook, open, pairs = debug.getinfo, debug.sethook, io.open, pairs
local hits = {}
sethook(function(_, l)
  local s = getinfo(2, "S").source
  if s:find("components/") then local t = hits[s] or {}; hits[s] = t; t[l] = true end
end, "l")
local function flush()
  sethook()
  local f = open(COVOUT, "a")
  for s, t in pairs(hits) do for l in pairs(t) do f:write(s:sub(2), "\t", l, "\n") end end
  f:close(); hits = {}
end
local rexit = os.exit
os.exit = function(...) flush(); return rexit(...) end
local script = arg[1]; arg = {[0] = script, table.unpack(arg, 2)}
local ok, err = pcall(dofile, script)
flush()
if not ok then io.stderr:write(tostring(err), "\n"); rexit(1) end
