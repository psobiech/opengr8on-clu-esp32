-- Synthetic OM.LUA in the format Object Manager generates for a real CLU_ZWAVE_2.
-- FwType 00000003
-- FwVersion 00000201
-- HwType 00000013
-- HwVersion 00000001

--STORAGE
STO99999 = OBJECT:new(44, "STO99999")

-- setVar type support
_type = {}
CLU100000001 = OBJECT:new(0, 0xC0A80164, "CLU100000001")
-- NAME_CLU CLU_TEST=CLU100000001

CLU100000002 = OBJECT:new(1, 0xC0A80165, "CLU100000002")

-- MODULES
mm_330000001 = OBJECT:new(2, 0x13ab6c81, 0x1e, "13ab6c81") -- 330000001 02 02
mm_550000001 = OBJECT:new(2, 0x20c85581, 0x2a, "20c85581") -- 550000001 02 03

-- IO_MODULES
 DOU0001 = OBJECT:new(4, mm_330000001, 0, "DOU0001")
-- NAME_IO LIGHT_A=DOU0001
 DOU0002 = OBJECT:new(4, mm_330000001, 1, "DOU0002")
 DIN0001 = OBJECT:new(3, mm_330000001, 0, "DIN0001")
 POW0001 = OBJECT:new(12, mm_330000001, 0, "POW0001")
 ROL0001 = OBJECT:new(24, mm_550000001, 0, "ROL0001")

function getVar(name)
    return _G[name]
end

function EventsFor_DIN0001_0()
    DOU0001:execute(0)
end
DIN0001:add_event(0, EventsFor_DIN0001_0)

function OnInit()
-- INIT_CLU_OBJECTS
CLU100000001:set(18, 1)
CLU100000001:set(22, "pool.ntp.org")
DOU0001:set(0, 0)
DIN0001:set(2, 500)
ROL0001:set(1, 30000)
initDone = true
end

CLU100000001:add_event(0, OnInit)
