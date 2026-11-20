-- TF-Bus emulator check: OM.LUA-style declaration of the emulated DIMMER_MOSFET_DIN (run_tfbus.py).
local nonce, serial_number = "@NONCE@", math.tointeger(tonumber("@SN@"))
local module_name = "mm_" .. serial_number
_G[module_name] = OBJECT:new(2, serial_number, 0x1a, string.format("%08x", serial_number))
DIM0001 = OBJECT:new(5, _G[module_name], 0, "DIM0001")

-- OnValueChange: every Value the CLU takes, from a script or from the module's status.
DIM0001:add_event(0, function()
  print("TFBUS_VALUE " .. nonce .. " " .. DIM0001:get(0))
end)

function tfbus_set(value)
  DIM0001:set(0, value)
  return "set"
end

SYSTEM.Init()
print("TFBUS_READY " .. nonce)
