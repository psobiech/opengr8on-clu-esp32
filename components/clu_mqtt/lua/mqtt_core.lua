-- @file mqtt_core.lua
-- ---------------------------------------------------------------------------------------------
-- Entity registry
-- ---------------------------------------------------------------------------------------------
local overrides = {}  -- object name -> list of opts | false (hidden)
local entities = {}   -- built list
local by_base = {}    -- topic path under root ("DOU1234", "DOU1234_2", "system/clu/restart") -> entity
local by_object = {}  -- object name -> entities with events
local stateful = {}   -- object name -> true: has scanned (not sampled) states, its events trigger a scan
local labels, clu_label = nil, nil
local ready, dirty = false, true
-- State scan: every cfg.scan_ms as a fallback (values that change without an event), and early
-- (scan_soon) after an MQTT command or an event of an object with states, at most every SCAN_MIN_MS.
local SCAN_MIN_MS = 20
local next_scan, last_scan, scan_soon = 0, 0, false

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

local device, origin, module_devices
local module_mysensors_serials -- serials of the MySensors modules, per build
local object_devices -- object -> its own HA device (opts.subdevice), per build
local module_link_topics -- module device -> its link topic (availability of its entities)

-- TF-Bus versions of a module as HA device fields: sw_version, hw_version (nil, nil until the
-- module answered VERSION_QUERY).
local function module_versions(module_status)
  if not module_status or not module_status.fw_ver then return nil, nil end
  return module_status.fw_ver, tostring(module_status.hw_ver)
end

-- Hidden entity (no discovery) publishing a TF-Bus module's link, ON / OFF, on
-- <root>/system/tfbus/modules/<module>, the same topic as the diagnostic "TF-Bus link" entity:
-- the second availability topic of the module's entities, so it is published without diagnostics
-- too. It also notices versions that VERSION_QUERY reports after the build (the bus scan runs
-- on its own): the configs are rebuilt and resent with them.
local function add_module_link(module, module_dev)
  local base = "system/tfbus/modules/" .. module._name
  module_link_topics[module_dev] = ROOT .. "/" .. base
  entities[#entities + 1] = {
    obj = module,
    base = base,
    states = {
      [""] = function()
        local module_status = clu.tfbus_module(module._sn)
        if not module_status then return nil end
        local sw_version, hw_version = module_versions(module_status)
        if sw_version ~= module_dev.sw_version or hw_version ~= module_dev.hw_version then dirty = true end
        return module_status.online and "ON" or "OFF"
      end,
    },
  }
end

-- Hidden entity (no discovery, publishes nothing) of a module the bus master does not know yet
-- (not found on the bus when the entities were built): rebuild once it shows up, so its entities
-- get the link availability and its versions.
local function add_module_watch(module)
  entities[#entities + 1] = {
    obj = module,
    base = "system/tfbus/modules/" .. module._name,
    states = {
      [""] = function()
        if clu.tfbus_module(module._sn) then dirty = true end
      end,
    },
  }
end

-- JSON null: json() encodes any non-data value as null (a nil table field would be left out).
local JSON_NULL = function() end

-- HA device of an object on a module (opts.subdevice, e.g. a panel button): named after its label,
-- under its module's device, available with the module.
local function object_device(object, parent_dev)
  local object_dev = object_devices[object]
  if not object_dev then
    object_dev = {
      identifiers = { NODE .. "_" .. object._name },
      name = labels[object._name] or object._name,
      manufacturer = parent_dev.manufacturer,
      via_device = parent_dev.identifiers[1],
    }
    object_devices[object] = object_dev
    module_link_topics[object_dev] = module_link_topics[parent_dev]
  end
  return object_dev
end

-- Serials of the modules emulated for MySensors nodes (__clu.ms_slots, MySensors builds only).
local function mysensors_serials()
  local serials = {}
  for _, slot in ipairs(clu.ms_slots and clu.ms_slots() or {}) do serials[slot.sn] = true end
  return serials
end

-- Home Assistant device of a module object (class 2): CLU > module > its inputs / outputs.
-- Manufacturer: on-board modules (clu_localio) are emulated by this firmware, MySensors modules
-- stand for MySensors nodes, the rest are Grenton modules on the bus. Modules the bus master knows
-- (__clu.tfbus_module) get the link availability; one it does not know yet is watched.
local function module_device(module)
  local module_dev = module_devices[module]
  if not module_dev then
    local model = MODULE_MODELS[module._hwType] or string.format("Module 0x%02X", tonumber(module._hwType) or 0)
    local sn = tostring(module._sn)
    local on_board = clu.local_channels and clu.local_channels(module._sn) ~= nil
    local module_status = clu.tfbus_module and clu.tfbus_module(module._sn)
    module_dev = {
      identifiers = { NODE .. "_" .. module._name },
      name = model .. " " .. sn,
      manufacturer = on_board and "OpenGr8on" or module_mysensors_serials[module._sn] and "MySensors"
                     or "Grenton",
      model = model,
      serial_number = sn,
      via_device = NODE,
    }
    module_dev.sw_version, module_dev.hw_version = module_versions(module_status)
    module_devices[module] = module_dev
    if module_status then add_module_link(module, module_dev)
    elseif clu.tfbus_module then add_module_watch(module) end
  end
  return module_dev
end

-- Default entities of an object: per its module type (MODULE_DEFAULTS), else per its class.
local function default_entities(object)
  local module = object._module
  local module_defaults = type(module) == "table" and MODULE_DEFAULTS[module._hwType]
  return module_defaults and module_defaults[object._class] or DEFAULTS[object._class]
end

-- false for a channel of an on-board module (clu_localio) with no hardware behind it: OM sees
-- every object of the emulated module's interface (DOUT of the GPIO input module, its unused DIN,
-- unset analog inputs, PowerSupplyVoltage, AnalogOUT), these get no default entities.
local function has_hardware(object)
  local module = object._module
  if type(module) ~= "table" or module._sn == nil or not clu.local_channels then return true end
  local channel_masks = clu.local_channels(module._sn)
  if not channel_masks then return true end
  local channel = math.tointeger(object._channel)
  return channel ~= nil and channel >= 0 and ((channel_masks[object._class] or 0) >> channel) & 1 == 1
end

-- CLU user features: OM.LUA declares each one with its type (`_type[name] = "number"`, the
-- table its setVar uses). They are globals; the object below reads / writes one of them.
local USER_FEATURE_CLASS = -2
local USER_FEATURE_KINDS = { number = "number", string = "text", boolean = "switch" }

local function user_feature(feature_name, feature_type)
  local feature = { _name = feature_name, _class = USER_FEATURE_CLASS }
  function feature.get() return rawget(_G, feature_name) end
  function feature.set(_, _, value)
    if feature_type == "number" then
      value = tonumber(value)
      if value == nil then return end
    elseif feature_type == "boolean" then
      value = value == true or (type(value) == "number" and value ~= 0)
    else
      value = tostring(value)
    end
    local set_var = rawget(_G, "setVar") -- OM's, applies _type
    if type(set_var) == "function" then set_var(feature_name, value) else rawset(_G, feature_name, value) end
  end
  return feature
end

-- Declared user features, sorted by name: { name = ..., type = ... }.
local function user_features()
  local feature_types, features = rawget(_G, "_type"), {}
  if type(feature_types) ~= "table" then return features end
  for feature_name, feature_type in pairs(feature_types) do
    if type(feature_name) == "string" and USER_FEATURE_KINDS[feature_type] then
      features[#features + 1] = { name = feature_name, type = feature_type }
    end
  end
  table.sort(features, function(left, right) return left.name < right.name end)
  return features
end

-- Entity options placed under a topic group ("var/<name>") unless they name one.
local function in_group(entity_options, group)
  local grouped = {}
  for option_index, options in ipairs(entity_options) do
    if options.group then
      grouped[option_index] = options
    else
      local grouped_options = { group = group }
      for key, value in pairs(options) do grouped_options[key] = value end
      grouped[option_index] = grouped_options
    end
  end
  return grouped
end

local function build()
  if not labels then load_labels() end
  device = {
    identifiers = { NODE },
    name = clu_label or ("CLU" .. cfg.sn),
    manufacturer = "OpenGr8on",
    model = cfg.model,
    hw_version = cfg.hw,
    serial_number = tostring(cfg.sn),
    sw_version = cfg.fw ~= "" and cfg.fw or nil,
  }
  origin = { name = "opengr8on-clu-esp32", sw_version = device.sw_version }
  entities, by_base, by_object, stateful, module_devices, module_link_topics = {}, {}, {}, {}, {}, {}
  module_mysensors_serials = mysensors_serials()
  object_devices = {}

  local function add(name, object, entity_options)
    for entity_index, options in ipairs(entity_options or {}) do
      local kind = KINDS[options.kind]
      if not kind then
        clu.log(2, "mqtt: " .. name .. ": unknown kind " .. tostring(options.kind))
      else
        local entity = kind(object, options)
        entity.obj = object
        local module = object._class == 2 and object or object._module -- module itself / its IO
        if type(module) == "table" and module._sn ~= nil then entity.device = module_device(module) end
        if options.subdevice and entity.device then entity.device = object_device(object, entity.device) end
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
        -- opts.name replaces the object's label, opts.name_suffix follows it ("Sun sunrise"). On the
        -- object's own device (opts.subdevice) HA prefixes the device name (the label) itself: the
        -- suffix alone, or null = named after the device.
        if options.name then entity.name = options.name
        elseif options.subdevice then entity.name = options.name_suffix or JSON_NULL
        elseif options.name_suffix then entity.name = (labels[name] or name) .. " " .. options.name_suffix
        else entity.name = labels[name] or entity.default_name or name end
        if options.device_class then entity.disc.device_class = options.device_class end
        if options.icon then entity.disc.icon = options.icon end
        if options.entity_category then entity.disc.entity_category = options.entity_category end
        if options.sampled then entity.every = cfg.diag_ms end -- noisy values: sampled, not scanned
        entities[#entities + 1] = entity
        if entity.cmds then by_base[entity.base] = entity end
        if entity.states and not entity.every then stateful[name] = true end
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
    local object = objects[name]
    local list = overrides[name]
    if list == nil and has_hardware(object) then list = default_entities(object) end
    add(name, object, list)
  end

  for _, feature in ipairs(user_features()) do
    local list = overrides[feature.name]
    if list == nil then list = { { kind = USER_FEATURE_KINDS[feature.type] } } end
    if list then add(feature.name, user_feature(feature.name, feature.type), in_group(list, "var")) end
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

-- ---------------------------------------------------------------------------------------------
-- Device-based discovery (Home Assistant 2024.11+): one retained config per HA device,
-- "<prefix>/device/<object_id>/config", its entities in "cmps". The CLU device is split by entity
-- source into configs that share its "dev" ids: objects (NODE), user features (NODE_var) and
-- firmware internals (NODE_system); a module is one config (its device id). An entity never moves
-- between configs. Component keys are "<platform>_<entity id>": a remapped object (switch ->
-- light) is a new component and the old one gets removed. Keys are abbreviated as in HA's
-- abbreviations.py; keys missing from these tables have no abbreviation and pass unchanged.
-- ---------------------------------------------------------------------------------------------
local COMPONENT_ABBREVIATIONS = {
  action_template = "act_tpl", action_topic = "act_t", availability = "avty",
  availability_mode = "avty_mode", availability_topic = "avty_t", color_temp_kelvin = "clr_temp_k",
  command_topic = "cmd_t", current_temperature_template = "curr_temp_tpl",
  current_temperature_topic = "curr_temp_t", device_class = "dev_cla", entity_category = "ent_cat",
  event_types = "evt_typ", icon = "ic", max_kelvin = "max_k", min_kelvin = "min_k",
  mode_command_topic = "mode_cmd_t", mode_state_template = "mode_stat_tpl",
  mode_state_topic = "mode_stat_t", options = "ops", payload_available = "pl_avail",
  payload_not_available = "pl_not_avail", position_topic = "pos_t",
  preset_mode_command_topic = "pr_mode_cmd_t", preset_mode_state_topic = "pr_mode_stat_t",
  preset_mode_value_template = "pr_mode_val_tpl", preset_modes = "pr_modes",
  set_position_topic = "set_pos_t", state_class = "stat_cla", state_stopped = "stat_stopped",
  state_topic = "stat_t", supported_color_modes = "sup_clrm",
  temperature_command_topic = "temp_cmd_t", temperature_state_template = "temp_stat_tpl",
  temperature_state_topic = "temp_stat_t", topic = "t", unique_id = "uniq_id",
  unit_of_measurement = "unit_of_meas",
}
local DEVICE_ABBREVIATIONS = { identifiers = "ids", manufacturer = "mf", model = "mdl",
                               hw_version = "hw", sw_version = "sw", serial_number = "sn" }
local ORIGIN_ABBREVIATIONS = { sw_version = "sw" }

local function abbreviated(fields, abbreviations)
  local short_fields = {}
  for key, value in pairs(fields) do short_fields[abbreviations[key] or key] = value end
  return short_fields
end

-- Discovery configs of the current entities, in build order: { topic, device, link_topic,
-- entities = {[component key] = entity} }.
local function discovery_configs()
  local configs_by_id, configs = {}, {}
  for _, entity in ipairs(entities) do
    if entity.comp then -- module links have no component
      local object_id = entity.device and entity.device.identifiers[1]
                        or entity.obj == SYSOBJ and NODE .. "_system"
                        or entity.obj._class == USER_FEATURE_CLASS and NODE .. "_var"
                        or NODE
      local discovery_config = configs_by_id[object_id]
      if not discovery_config then
        discovery_config = { topic = cfg.prefix .. "/device/" .. object_id .. "/config",
                             device = entity.device or device,
                             link_topic = entity.device and module_link_topics[entity.device],
                             entities = {} }
        configs_by_id[object_id] = discovery_config
        configs[#configs + 1] = discovery_config
      end
      discovery_config.entities[entity.comp .. "_" .. entity.id] = entity
    end
  end
  return configs
end

local function component_payload(entity)
  local component = { p = entity.comp, ["~"] = ROOT .. "/" .. entity.base, name = entity.name,
                      unique_id = entity.uid }
  for key, value in pairs(entity.disc) do component[key] = value end
  -- The link itself: available with the CLU, whatever the link says (overrides the device's).
  if entity.link then component.availability = { { t = cfg.status } } end
  return abbreviated(component, COMPONENT_ABBREVIATIONS)
end

-- Available while the CLU is online (LWT) and, for a TF-Bus module, its link is ON. removed:
-- {[component key] = platform} of components published before and gone now, sent as {"p": ...}
-- once so HA deletes their entities (HA keeps components a config just leaves out).
local function config_payload(discovery_config, removed)
  local payload = { dev = abbreviated(discovery_config.device, DEVICE_ABBREVIATIONS),
                    o = abbreviated(origin, ORIGIN_ABBREVIATIONS), cmps = {} }
  if discovery_config.link_topic then
    payload.avty = { { t = cfg.status },
                     { t = discovery_config.link_topic, pl_avail = "ON", pl_not_avail = "OFF" } }
    payload.avty_mode = "all"
  else
    payload.avty_t = cfg.status
  end
  for component_key, entity in pairs(discovery_config.entities) do
    payload.cmps[component_key] = component_payload(entity)
  end
  for component_key, platform in pairs(removed) do payload.cmps[component_key] = { p = platform } end
  return json(payload)
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

-- Persisted (mqtt.devices): {[config topic] = {[component key] = platform}} as last published.
local function publish_discovery()
  if not cfg.ha then return end
  local persisted_mqtt = persist.section("mqtt")
  local changed = false
  -- Per-entity configs of earlier firmware (mqtt.configs): cleared before the device configs
  -- reuse their unique_ids.
  if persisted_mqtt.configs then
    for _, topic in ipairs(persisted_mqtt.configs) do enqueue(topic, "", true, 1, true) end
    persisted_mqtt.configs, changed = nil, true
  end
  local published = persisted_mqtt.devices or {}
  local current, payloads, removing = {}, {}, {}
  for _, discovery_config in ipairs(discovery_configs()) do
    local platforms, removed = {}, {}
    for component_key, entity in pairs(discovery_config.entities) do platforms[component_key] = entity.comp end
    for component_key, platform in pairs(published[discovery_config.topic] or {}) do
      if not platforms[component_key] then removed[component_key] = platform end
    end
    current[discovery_config.topic] = platforms
    payloads[#payloads + 1] = { topic = discovery_config.topic, payload = config_payload(discovery_config, removed) }
    removing[discovery_config.topic] = next(removed) ~= nil
  end
  -- Removals first: an entity moving to another config (e.g. onto its own sub-device) keeps its
  -- unique_id, which HA only accepts once the old entity is gone. A whole device gone (module
  -- removed from OM, all user features / internals hidden): empty config, HA deletes its entities.
  for topic in pairs(published) do
    if not current[topic] then enqueue(topic, "", true, 1, true) end
  end
  for _, removals_first in ipairs({ true, false }) do
    for _, config_message in ipairs(payloads) do
      if removing[config_message.topic] == removals_first then
        enqueue(config_message.topic, config_message.payload, true, 1, true)
      end
    end
  end
  if changed or __ser(current) ~= __ser(published) then
    persisted_mqtt.devices = current
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
  if stateful[self._name] then scan_soon = true end -- OnValueChange & co: publish the new state now
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
      scan_soon = true
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
  scan_soon = true
end

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
  if ready and (now_ms >= next_scan or (scan_soon and now_ms - last_scan >= SCAN_MIN_MS)) then
    next_scan, last_scan, scan_soon = now_ms + cfg.scan_ms, now_ms, false
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
    error("MQTT.expose: kind must be one of switch, light, binary_sensor, sensor, number, text, select, climate, cover, event, button, module", 2)
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
