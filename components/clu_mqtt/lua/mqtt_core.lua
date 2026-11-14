-- @file mqtt_core.lua
-- ---------------------------------------------------------------------------------------------
-- Entity registry
-- ---------------------------------------------------------------------------------------------
local overrides = {}  -- object name -> list of opts | false (hidden)
local entities = {}   -- built list
local by_base = {}    -- topic path under root ("DOU1234", "DOU1234_2", "system/clu/restart") -> entity
local by_object = {}  -- object name -> entities with events
local labels, clu_label = nil, nil
local ready, dirty = false, true

local function load_labels()
  labels = {}
  for _, file_name in ipairs({ "OM.LUA", "om.lua" }) do
    local label_file = io.open((clu.root or ".") .. "/" .. file_name, "rb")
    if label_file then
      for line in label_file:lines() do
        local kind, label, id = line:match("^%s*%-%-%s*NAME_(%u+)%s+(.-)=([%w_]+)%s*$")
        if kind then
          label = label:gsub("_", " ")
          labels[id] = label
          if kind == "CLU" then clu_label = label end
        end
      end
      label_file:close()
      break
    end
  end
end

local device, origin

local function build()
  if not labels then load_labels() end
  device = {
    identifiers = { NODE },
    name = clu_label or ("CLU" .. cfg.sn),
    manufacturer = "OpenGr8on",
    model = "ESP32-S3 CLU",
    serial_number = tostring(cfg.sn),
    sw_version = cfg.fw,
  }
  origin = { name = "opengr8on-clu-esp32" }
  entities, by_base, by_object = {}, {}, {}

  local function add(name, object, entity_options)
    for entity_index, options in ipairs(entity_options or {}) do
      local kind = KINDS[options.kind]
      if not kind then
        clu.log(2, "mqtt: " .. name .. ": unknown kind " .. tostring(options.kind))
      else
        local entity = kind(object, options)
        entity.obj = object
        -- e.base: topic path under root. First state-bearing entity of an object owns
        -- <root>/<obj>; later ones get <obj>_<n>; opts.group prefixes a path ("system/memory").
        local base = options.base or name
        if not options.base and entity_index > 1 and not entity.topic then
          base = name .. "_" .. entity_index
        end
        if options.group then base = options.group .. "/" .. base end
        entity.base = base
        entity.id = base:gsub("/", "_") .. (entity.suffix and ("_" .. entity.suffix) or "")
        entity.uid = NODE .. "_" .. entity.id
        -- Leaf: the single state is published on <root>/<base> itself, not <root>/<base>/state.
        if options.leaf and entity.states and entity.states.state then
          entity.states[""], entity.states.state = entity.states.state, nil
          entity.disc.state_topic = "~"
        end
        entity.name = options.name or labels[name] or entity.default_name or name
        if options.device_class then entity.disc.device_class = options.device_class end
        if options.icon then entity.disc.icon = options.icon end
        if options.entity_category then entity.disc.entity_category = options.entity_category end
        if options.sampled then entity.every = cfg.diag_ms end -- noisy values: sampled, not scanned
        entities[#entities + 1] = entity
        if entity.cmds then by_base[entity.base] = entity end
        if entity.events then
          local object_event_entities = by_object[name] or {}
          object_event_entities[#object_event_entities + 1] = entity
          by_object[name] = object_event_entities
        end
      end
    end
  end

  local names = {}
  for name in pairs(objects) do names[#names + 1] = name end
  table.sort(names)
  for _, name in ipairs(names) do
    local list = overrides[name]
    if list == nil then list = DEFAULTS[objects[name]._class] end
    add(name, objects[name], list)
  end

  if cfg.diag_ms and overrides[SYS_NAME] ~= false then
    local list = {}
    for _, opts in ipairs(SYSTEM_ENTITIES) do
      opts.entity_category = opts.entity_category or DIAG
      if opts.leaf == nil then opts.leaf = opts.kind ~= "button" end
      list[#list + 1] = opts
    end
    for _, opts in ipairs(overrides[SYS_NAME] or {}) do list[#list + 1] = opts end
    add(SYS_NAME, SYSOBJ, list)
  end
  dirty = false
end

local function state_topic(entity, state_key)
  return ROOT .. "/" .. entity.base .. (state_key == "" and "" or "/" .. state_key)
end

local function config_topic(entity)
  return cfg.prefix .. "/" .. entity.comp .. "/" .. NODE .. "/" .. entity.id .. "/config"
end

local function config_payload(entity)
  local discovery = { ["~"] = ROOT .. "/" .. entity.base, name = entity.name, unique_id = entity.uid,
              availability_topic = cfg.status, device = device, origin = origin }
  for key, value in pairs(entity.disc) do discovery[key] = value end
  return json(discovery)
end

-- ---------------------------------------------------------------------------------------------
-- Outbound queue: paced into esp-mqtt's outbox. Retained states coalesce per topic (only the
-- latest value of a pending topic is sent), so they are bounded by the topic count. Events (and
-- API replies) never coalesce: at most EVENTS_MAX of them wait, newer ones are dropped (and
-- counted) while the queue is full, so a publisher that cannot keep up never exhausts the heap.
-- ---------------------------------------------------------------------------------------------
local outbound_queue, queue_head, queue_tail, coalesced_entries_by_topic = {}, 1, 0, {}
local pending_event_count, dropped = 0, 0 -- pending non-coalesced entries; events dropped in this overflow
local last = {}       -- topic -> last published retained state

-- Forget what was published: every state goes out again on the next scan (sampled ones too).
local function republish()
  last = {}
  for _, entity in ipairs(entities) do entity.due = nil end
end
local connected, connection_generation = false, nil

local function reset_queue()
  outbound_queue, queue_head, queue_tail, coalesced_entries_by_topic, pending_event_count = {}, 1, 0, {}, 0
end

local function enqueue(topic, payload, retain, qos, coalesce)
  if coalesce then
    local existing_entry = coalesced_entries_by_topic[topic]
    if existing_entry then existing_entry.payload = payload; return end
  elseif pending_event_count >= EVENTS_MAX then
    if dropped == 0 then clu.log(2, "mqtt: event queue full, dropping new events") end
    dropped = dropped + 1
    return
  else
    pending_event_count = pending_event_count + 1
    if dropped > 0 then
      clu.log(2, "mqtt: " .. dropped .. " event(s) dropped")
      dropped = 0
    end
  end
  local queue_entry = { topic = topic, payload = payload, retain = retain, qos = qos,
                        coalesce = coalesce }
  queue_tail = queue_tail + 1
  outbound_queue[queue_tail] = queue_entry
  if coalesce then coalesced_entries_by_topic[topic] = queue_entry end
end

local function flush()
  local sent_count = 0
  while queue_head <= queue_tail and sent_count < PUBS_PER_TICK
      and clu.mqtt_outbox() < OUTBOX_MAX do
    local queue_entry = outbound_queue[queue_head]
    outbound_queue[queue_head] = nil -- free sent entries now: under steady load the queue may never fully drain
    queue_head = queue_head + 1
    if queue_entry.coalesce then
      if coalesced_entries_by_topic[queue_entry.topic] == queue_entry then
        coalesced_entries_by_topic[queue_entry.topic] = nil
      end
    else
      pending_event_count = pending_event_count - 1
    end
    if not clu.mqtt_publish(queue_entry.topic, queue_entry.payload, queue_entry.retain,
                            queue_entry.qos) then
      reset_queue() -- link dropped: everything is resent on the next connect
      return
    end
    sent_count = sent_count + 1
  end
  if queue_head > queue_tail then reset_queue() end
end

local function publish_discovery()
  if not cfg.ha then return end
  local persisted_mqtt = persist.section("mqtt")
  local current_topics = {}
  for _, entity in ipairs(entities) do
    local topic = config_topic(entity)
    current_topics[#current_topics + 1] = topic
    enqueue(topic, config_payload(entity), true, 1, true)
  end
  -- Clear configs this CLU published before but no longer has (object removed / hidden / remapped).
  local keep = {}
  for _, topic in ipairs(current_topics) do keep[topic] = true end
  for _, topic in ipairs(persisted_mqtt.configs or {}) do
    if not keep[topic] then enqueue(topic, "", true, 1, true) end
  end
  table.sort(current_topics)
  local changed = #current_topics ~= #(persisted_mqtt.configs or {})
  for topic_index, topic in ipairs(current_topics) do
    if not changed and persisted_mqtt.configs[topic_index] ~= topic then changed = true end
  end
  if changed then
    persisted_mqtt.configs = current_topics
    persist.mark()
  end
end

local function scan(now_ms)
  for _, entity in ipairs(entities) do
    local due = true
    if entity.every then
      due = now_ms >= (entity.due or 0)
      if due then entity.due = now_ms + entity.every end
    end
    if entity.states and due then
      for state_key, state_fn in pairs(entity.states) do
        local succeeded, state_value = pcall(state_fn)
        if succeeded and state_value ~= nil then
          local topic = state_topic(entity, state_key)
          if last[topic] ~= state_value then
            last[topic] = state_value
            enqueue(topic, state_value, true, 0, true)
          end
        end
      end
    end
  end
end

-- Events: OBJECT._fire is the one path every Grenton event takes (driver events via __fire and
-- virtual objects calling o:_fire), so wrap it.
local orig_fire = OBJECT._fire
function OBJECT:_fire(event_index)
  orig_fire(self, event_index)
  if not connected then return end
  local event_entities = by_object[self._name]
  if not event_entities then return end
  for _, entity in ipairs(event_entities) do
    local event_type = entity.events[event_index]
    if event_type then
      enqueue(ROOT .. "/" .. entity.base .. "/event", json({ event_type = event_type }),
              false, 0, false)
    end
  end
end

-- ---------------------------------------------------------------------------------------------
-- Raw object API (cfg.api = menuconfig CLU_MQTT_OBJECT_API). Payloads are JSON values, decoded
-- here (never evaluated as Lua):
--   <root>/<OBJ>/feature/<idx>/get   -> <root>/<OBJ>/feature/<idx>  {"ok":true,"value":value}
--   <root>/<OBJ>/feature/<idx>/set   value -> same reply with the value read back
--   <root>/<OBJ>/execute/<idx>       [args] | value | "" -> <root>/<OBJ>/execute/<idx>/result
-- Remote-CLU proxies (class 1) are refused: their execute(0, source) runs source as Lua on the peer.
-- ---------------------------------------------------------------------------------------------
local function api_reply(reply_topic, succeeded, value)
  local body = succeeded and ('{"ok":true,"value":' .. json(value) .. "}")
               or ('{"ok":false,"error":' .. json(tostring(value)) .. "}")
  enqueue(reply_topic, body, false, 0, false)
end

local function api_target(object_name, feature_index)
  local object = objects[object_name]
  if not object then return nil, "unknown object " .. object_name end
  if object._class == 1 then return nil, "remote CLU objects are not reachable over MQTT" end
  feature_index = math.tointeger(tonumber(feature_index))
  if not feature_index or feature_index < 0 or feature_index > 255 then return nil, "bad index" end
  return object, feature_index
end

-- Payload -> value: JSON when it parses, else the text itself (so `set` accepts plain words).
local function api_value(payload)
  local value, decode_error = jdecode(payload)
  if decode_error then return payload end
  return value
end

local function api_message(command_path, payload)
  local object_name, feature_index, operation = command_path:match("^([^/]+)/feature/(%d+)/(%a+)$")
  if object_name and (operation == "get" or operation == "set") then
    local reply_topic = ROOT .. "/" .. object_name .. "/feature/" .. feature_index
    local object, index = api_target(object_name, feature_index)
    if not object then api_reply(reply_topic, false, index); return object_name end
    if operation == "set" then
      local value = api_value(payload)
      if type(value) == "table" then
        api_reply(reply_topic, false, "value must be a scalar")
        return object_name
      end
      local succeeded, call_error = pcall(object.set, object, index, value)
      if not succeeded then api_reply(reply_topic, false, call_error); return object_name end
    end
    api_reply(reply_topic, pcall(object.get, object, index))
    return object_name
  end
  object_name, feature_index = command_path:match("^([^/]+)/execute/(%d+)$")
  if object_name then
    local reply_topic = ROOT .. "/" .. object_name .. "/execute/" .. feature_index .. "/result"
    local object, index = api_target(object_name, feature_index)
    if not object then api_reply(reply_topic, false, index); return object_name end
    local arguments, argument_count = {}, 0
    if payload:match("%S") then
      local value = api_value(payload)
      if type(value) == "table" and value.n then
        arguments, argument_count = value, value.n
      elseif type(value) == "table" then
        api_reply(reply_topic, false, "args must be a JSON array")
        return object_name
      else
        arguments, argument_count = { value }, 1
      end
    end
    api_reply(reply_topic, pcall(object.execute, object, index,
                                 table.unpack(arguments, 1, argument_count)))
    return object_name
  end
  return false
end

local CMD_SUBS = { "temperature/set", "position/set", "preset/set", "mode/set", "set" }

local function on_message(topic, payload)
  if cfg.ha and topic == cfg.prefix .. "/status" then
    if payload == "online" then
      -- Home Assistant restarted: resend configs and all states.
      republish()
      publish_discovery()
    end
    return
  end
  if topic:sub(1, #ROOT + 1) ~= ROOT .. "/" then return end
  if cfg.api then
    local target = api_message(topic:sub(#ROOT + 2), payload)
    if target ~= false then
      -- The object may have changed: resend its entity states on the next scan.
      for _, entity in ipairs(entities) do
        if entity.obj == objects[target] then
          for state_key in pairs(entity.states or {}) do
            last[state_topic(entity, state_key)] = nil
          end
        end
      end
      return
    end
  end
  -- <root>/<path>/<cmd>: path may have levels ("system/clu/restart"), cmd is "set" / "position/set".
  local command_path, entity, command_handler = topic:sub(#ROOT + 2), nil, nil
  for _, command_suffix in ipairs(CMD_SUBS) do
    local base = command_path:sub(-#command_suffix - 1) == "/" .. command_suffix
                and command_path:sub(1, -#command_suffix - 2)
    entity = base and by_base[base]
    command_handler = entity and entity.cmds[command_suffix]
    if command_handler then break end
  end
  if not command_handler then return end
  payload = payload:match("^%s*(.-)%s*$")
  if not entity.raw then payload = payload:upper() end -- raw: JSON commands (lights)
  local succeeded, call_error = pcall(command_handler, payload)
  if not succeeded then clu.log(1, "mqtt: " .. topic .. ": " .. tostring(call_error)) end
  -- Commands that change nothing (e.g. ON on an output that is on) would otherwise leave an
  -- optimistic HA UI out of step: republish the current state on the next scan.
  for state_key in pairs(entity.states or {}) do last[state_topic(entity, state_key)] = nil end
end

local next_scan = 0

local function tick(now_ms)
  local is_connected, broker_generation = clu.mqtt_state()
  if not is_connected then
    if connected then reset_queue(); republish() end
    connected = false
    return
  end
  connected = true
  if dirty and ready then
    build()
    if broker_generation == connection_generation then publish_discovery() end
  end
  if broker_generation ~= connection_generation then
    -- (Re)connected: the broker session is new, republish configs and every state.
    connection_generation = broker_generation
    reset_queue()
    republish()
    if ready then publish_discovery() end
  end
  for _ = 1, RECV_PER_TICK do
    local received_topic, received_payload = clu.mqtt_recv()
    if not received_topic then break end
    on_message(received_topic, received_payload)
  end
  if ready and now_ms >= next_scan then
    next_scan = now_ms + cfg.scan_ms
    scan(now_ms)
  end
  flush()
end

HOOKS.init[#HOOKS.init + 1] = function() ready = true; dirty = true end
HOOKS.loop[#HOOKS.loop + 1] = tick

-- ---------------------------------------------------------------------------------------------
-- Script API
-- ---------------------------------------------------------------------------------------------
local function obj_name(object)
  if type(object) == "table" then return object._name end
  return tostring(object)
end

MQTT = { root = ROOT }

function MQTT.expose(object, options)
  local name = obj_name(object)
  if type(options) == "string" then options = { kind = options } end
  if type(options) ~= "table" or not KINDS[options.kind] then
    error("MQTT.expose: kind must be one of switch, light, binary_sensor, sensor, climate, cover, event, button, module", 2)
  end
  local override_list = overrides[name]
  if not override_list then override_list = {}; overrides[name] = override_list end
  override_list[#override_list + 1] = options
  dirty = true
end

function MQTT.hide(object)
  overrides[obj_name(object)] = false
  dirty = true
end

function MQTT.connected() return (clu.mqtt_state()) end

function MQTT.publish(topic, payload, retain)
  return clu.mqtt_publish(tostring(topic), tostring(payload), retain and true or false, 0)
end

__mqtt = { json = json, jdecode = jdecode, entities = function() return entities end } -- for tests / debugging
