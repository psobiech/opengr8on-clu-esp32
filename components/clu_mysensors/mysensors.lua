-- MySensors network for the scripts. Loaded after objects.lua (component clu_mysensors, menuconfig
-- CLU_MYSENSORS). The C side (clu_mysensors.c) is the controller: it hands out node ids, answers
-- I_CONFIG / I_TIME and C_REQ (from the last value set) and keeps the node registry; every
-- message also lands here, dispatched on SYSTEM.Loop.
--
-- USER.LUA API:
--   MySensors.on(node, child, vtype, fn)    fn(value, msg) on C_SET from a node; nil = any.
--                                           value: number, or string for text types (V_TEXT,
--                                           V_RGB, ...). Returns a handle for MySensors.off().
--   MySensors.onMessage(fn)                 fn(msg) on every message (msg.node, child, cmd, ack,
--                                           type, payload); returns a handle
--   MySensors.off(handle)
--   MySensors.send(node, child, vtype, value [, ack])     C_SET to a node (true -> 1, false -> 0)
--   MySensors.request(node, child, vtype)   C_REQ: the node answers with a C_SET
--   MySensors.internal(node, child, itype [, payload])    C_INTERNAL (e.g. I_REBOOT)
--   MySensors.raw(node, child, cmd, type [, payload [, ack]])
--   MySensors.get(node, child, vtype)       -> value, age_ms (last value seen / sent) | nil
--   MySensors.nodes()                       -> { [id] = { sketch, version, lib, type, battery,
--                                                age_ms, children = { [child] = { type, desc } } } }
--   MySensors.status()                      -> { connected, version, rx, tx, dropped, bad, rx_bytes,
--                                                link = "unknown" | "silent" | "echo" | "garbage" | "ok",
--                                                trace }
--   MySensors.trace(on)                     log every gateway line (both directions) at INFO
--   MySensors.slots()                       -> { { module, sn, kind = "din" | "dout", channel,
--                                                node, child, vtype } }: sensors bound to channels
--                                                of the emulated IO_MODULE_DIN_8 modules in OM
--   MySensors.unmap(node [, child])         free a node's channels (e.g. node removed) -> count
--   MySensors.C / S / V / I                 command, presentation, variable, internal types
--
-- Example:
--   MySensors.on(12, 1, MySensors.V.V_TEMP, function(t) CLU_TEMP:set(0, t) end)
--   MySensors.send(13, 7, MySensors.V.V_STATUS, true)
local C = __clu
local HOOKS = __hooks

local RECV_PER_TICK = 16

local M = {}

M.C = { C_PRESENTATION = 0, C_SET = 1, C_REQ = 2, C_INTERNAL = 3, C_STREAM = 4 }

M.S = {
  S_DOOR = 0, S_MOTION = 1, S_SMOKE = 2, S_LIGHT = 3, S_BINARY = 3, S_DIMMER = 4, S_COVER = 5,
  S_TEMP = 6, S_HUM = 7, S_BARO = 8, S_WIND = 9, S_RAIN = 10, S_UV = 11, S_WEIGHT = 12,
  S_POWER = 13, S_HEATER = 14, S_DISTANCE = 15, S_LIGHT_LEVEL = 16, S_ARDUINO_NODE = 17,
  S_ARDUINO_REPEATER_NODE = 18, S_LOCK = 19, S_IR = 20, S_WATER = 21, S_AIR_QUALITY = 22,
  S_CUSTOM = 23, S_DUST = 24, S_SCENE_CONTROLLER = 25, S_RGB_LIGHT = 26, S_RGBW_LIGHT = 27,
  S_COLOR_SENSOR = 28, S_HVAC = 29, S_MULTIMETER = 30, S_SPRINKLER = 31, S_WATER_LEAK = 32,
  S_SOUND = 33, S_VIBRATION = 34, S_MOISTURE = 35, S_INFO = 36, S_GAS = 37, S_GPS = 38,
}

M.V = {
  V_TEMP = 0, V_HUM = 1, V_STATUS = 2, V_LIGHT = 2, V_PERCENTAGE = 3, V_DIMMER = 3,
  V_PRESSURE = 4, V_FORECAST = 5, V_RAIN = 6, V_RAINRATE = 7, V_WIND = 8, V_GUST = 9,
  V_DIRECTION = 10, V_UV = 11, V_WEIGHT = 12, V_DISTANCE = 13, V_IMPEDANCE = 14, V_ARMED = 15,
  V_TRIPPED = 16, V_WATT = 17, V_KWH = 18, V_SCENE_ON = 19, V_SCENE_OFF = 20,
  V_HVAC_FLOW_STATE = 21, V_HVAC_SPEED = 22, V_LIGHT_LEVEL = 23, V_VAR1 = 24, V_VAR2 = 25,
  V_VAR3 = 26, V_VAR4 = 27, V_VAR5 = 28, V_UP = 29, V_DOWN = 30, V_STOP = 31, V_IR_SEND = 32,
  V_IR_RECEIVE = 33, V_FLOW = 34, V_VOLUME = 35, V_LOCK_STATUS = 36, V_LEVEL = 37,
  V_VOLTAGE = 38, V_CURRENT = 39, V_RGB = 40, V_RGBW = 41, V_ID = 42, V_UNIT_PREFIX = 43,
  V_HVAC_SETPOINT_COOL = 44, V_HVAC_SETPOINT_HEAT = 45, V_HVAC_FLOW_MODE = 46, V_TEXT = 47,
  V_CUSTOM = 48, V_POSITION = 49, V_IR_RECORD = 50,
}

M.I = {
  I_BATTERY_LEVEL = 0, I_TIME = 1, I_VERSION = 2, I_ID_REQUEST = 3, I_ID_RESPONSE = 4,
  I_INCLUSION_MODE = 5, I_CONFIG = 6, I_FIND_PARENT = 7, I_FIND_PARENT_RESPONSE = 8,
  I_LOG_MESSAGE = 9, I_CHILDREN = 10, I_SKETCH_NAME = 11, I_SKETCH_VERSION = 12, I_REBOOT = 13,
  I_GATEWAY_READY = 14, I_REQUEST_SIGNING = 15, I_GET_NONCE = 16, I_GET_NONCE_RESPONSE = 17,
  I_HEARTBEAT = 18, I_HEARTBEAT_REQUEST = 18, I_PRESENTATION = 19, I_DISCOVER = 20,
  I_DISCOVER_RESPONSE = 21, I_HEARTBEAT_RESPONSE = 22,
}

-- V_ types whose payload is text even when it looks like a number ("1e3" as an RGB value, ids)
local TEXT = {}
for _, k in ipairs({ "V_FORECAST", "V_HVAC_FLOW_STATE", "V_HVAC_SPEED", "V_IR_SEND", "V_IR_RECEIVE",
                     "V_RGB", "V_RGBW", "V_ID", "V_UNIT_PREFIX", "V_HVAC_FLOW_MODE", "V_TEXT",
                     "V_CUSTOM", "V_POSITION", "V_IR_RECORD" }) do
  TEXT[M.V[k]] = true
end

local function decode(vtype, payload)
  if TEXT[vtype] then return payload end
  return tonumber(payload) or payload
end

local function encode(v)
  if v == true then return "1" end
  if v == false or v == nil then return "0" end
  if type(v) == "number" then
    if v == math.floor(v) and v > -1e15 and v < 1e15 then return string.format("%d", v) end
    return string.format("%.6g", v)
  end
  return tostring(v)
end

-- handlers: { fn, node, child, vtype, raw }  (raw = every message)
local handlers = {}

local function add(h)
  handlers[#handlers + 1] = h
  return h
end

local function check_fn(fn)
  if type(fn) ~= "function" then error("MySensors: handler function expected", 3) end
end

function M.on(node, child, vtype, fn)
  check_fn(fn)
  return add({ fn = fn, node = node, child = child, vtype = vtype })
end

function M.onMessage(fn)
  check_fn(fn)
  return add({ fn = fn, raw = true })
end

function M.off(h)
  for i = #handlers, 1, -1 do
    if handlers[i] == h then table.remove(handlers, i) end
  end
end

function M.raw(node, child, cmd, typ, payload, ack)
  return C.ms_send(node, child, cmd, typ, payload ~= nil and encode(payload) or "", ack)
end

function M.send(node, child, vtype, value, ack)
  return C.ms_send(node, child, M.C.C_SET, vtype, encode(value), ack)
end

function M.request(node, child, vtype)
  return C.ms_send(node, child, M.C.C_REQ, vtype, "")
end

function M.internal(node, child, itype, payload)
  return M.raw(node, child, M.C.C_INTERNAL, itype, payload)
end

function M.get(node, child, vtype)
  local p, age = C.ms_get(node, child, vtype)
  if p == nil then return nil end
  return decode(vtype, p), age
end

M.nodes = C.ms_nodes
M.status = C.ms_status
M.trace = C.ms_trace
M.slots = C.ms_slots
M.unmap = C.ms_unmap

local function match(h, msg)
  if h.raw then return true end
  return msg.cmd == M.C.C_SET and (h.node == nil or h.node == msg.node) and
         (h.child == nil or h.child == msg.child) and (h.vtype == nil or h.vtype == msg.type)
end

local function dispatch(msg)
  local value = decode(msg.type, msg.payload)
  -- a copy: handlers may add / remove handlers
  local list = { table.unpack(handlers) }
  for _, h in ipairs(list) do
    if match(h, msg) then
      local ok, err
      if h.raw then ok, err = pcall(h.fn, msg) else ok, err = pcall(h.fn, value, msg) end
      if not ok then C.log(1, "MySensors handler: " .. tostring(err)) end
    end
  end
end

local function tick()
  for _ = 1, RECV_PER_TICK do
    local node, child, cmd, ack, typ, payload = C.ms_recv()
    if not node then break end
    dispatch({ node = node, child = child, cmd = cmd, ack = ack, type = typ, payload = payload })
  end
end

HOOKS.loop[#HOOKS.loop + 1] = tick

MySensors = M
