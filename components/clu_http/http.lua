-- GATE HTTP objects (interfaces object_http_request_v2.xml / object_http_listener_v2.xml), declared
-- in OM.LUA as GATE:new(121, "HTT1234") / GATE:new(120, "HTT5678"). Loaded after objects.lua.
--
-- HttpRequest (121): the C worker (__clu.http_send) runs the request; the result comes back through
-- __clu.http_poll at SYSTEM.Loop and fires OnResponse.
-- HttpListener (120): the C server accepts a request on the listener port and hands it over through
-- __clu.http_poll; it goes to the listener whose Path equals the request path, OnRequest fires and
-- SendResponse answers it (__clu.http_respond). No listener for the path, or no OnRequest handler:
-- 404. No SendResponse in time: the C side answers 504.
--
-- Body types (RequestType / ResponseType): 0 None, 1 Text, 2 JSON, 3 XML, 4 FormData, 5 Other.
-- JSON / XML / FormData bodies are parsed into Lua tables on receipt and tables are serialized when
-- sent; a string is always sent as it is. Header and query features take a table or a string
-- ("Name: value\r\n..." / "a=1&b=2").
local clu = __clu
local OBJECTS = __objects
local HOOKS = __hooks
local defclass = __defclass
local json, jdecode = __json.encode, __json.decode

local T_NONE, T_TEXT, T_JSON, T_XML, T_FORM, T_OTHER = 0, 1, 2, 3, 4, 5
local CONTENT_TYPE = {
  [T_TEXT] = "text/plain",
  [T_JSON] = "application/json",
  [T_XML] = "text/xml",
  [T_FORM] = "application/x-www-form-urlencoded",
}
local POLL_PER_TICK = 8

-- ---------------------------------------------------------------------------------------------
-- Codecs
-- ---------------------------------------------------------------------------------------------
local function url_encode(text)
  return (tostring(text):gsub("[^%w%-%._~]", function(character) return string.format("%%%02X", character:byte()) end))
end

local function url_decode(text)
  text = text:gsub("%+", " ")
  return (text:gsub("%%(%x%x)", function(hex_value) return string.char(tonumber(hex_value, 16)) end))
end

local function sorted_keys(input_table)
  local keys = {}
  for k in pairs(input_table) do keys[#keys + 1] = k end
  table.sort(keys, function(left, right) return tostring(left) < tostring(right) end)
  return keys
end

local function form_encode(fields)
  local parts = {}
  for _, key in ipairs(sorted_keys(fields)) do
    parts[#parts + 1] = url_encode(key) .. "=" .. url_encode(fields[key])
  end
  return table.concat(parts, "&")
end

local function form_decode(query)
  local fields = {}
  for part in query:gmatch("[^&]+") do
    local key, value = part:match("^([^=]*)=(.*)$")
    if key then fields[url_decode(key)] = url_decode(value) else fields[url_decode(part)] = "" end
  end
  return fields
end

local XML_ESC = { ["&"] = "&amp;", ["<"] = "&lt;", [">"] = "&gt;", ['"'] = "&quot;", ["'"] = "&apos;" }
local XML_UNESC = { amp = "&", lt = "<", gt = ">", quot = '"', apos = "'" }

local function xml_unescape(text)
  return (text:gsub("&(#?x?)(%w+);", function(prefix, codepoint_text)
    if prefix == "" then return XML_UNESC[codepoint_text] end
    local codepoint = tonumber(codepoint_text, prefix == "#x" and 16 or 10)
    return codepoint and utf8.char(codepoint) or nil
  end))
end

local function xml_attrs(el, text)
  for key, quote, value in text:gmatch("([%a_][%w_:%.%-]*)%s*=%s*([\"'])(.-)%2") do
    el[key] = xml_unescape(value)
  end
end

-- XML text -> element tree (GATE HTTP manual): an element is a table with its tag name in
-- `xmlTag`, its attributes as fields and its children (elements, text) at [1], [2], ...; e.g.
-- <current><city id="7"><country>GB</country></city></current> -> resp[1].id == "7",
-- resp[1][1][1] == "GB", resp[1][1].xmlTag == "country". Returns the root element; whitespace-only
-- text is dropped. nil on malformed input.
local function xml_decode(text)
  text = text:gsub("<%?.-%?>", ""):gsub("<!%-%-.-%-%->", ""):gsub("<!DOCTYPE.->", "")
  local stack, root = { { } }, nil
  local pos = 1
  while true do
    local tag_start, tag_end, closing, name, attrs, selfclose = text:find("<(/?)([%a_][%w_:%.%-]*)([^>]-)(/?)>", pos)
    local cdata_start, cdata_end, cdata = text:find("<!%[CDATA%[(.-)%]%]>", pos)
    if cdata_start and (not tag_start or cdata_start < tag_start) then -- CDATA before the next tag: literal text
      local top = stack[#stack]
      local text_segment = text:sub(pos, cdata_start - 1)
      if text_segment:find("%S") then top[#top + 1] = xml_unescape(text_segment:match("^%s*(.-)%s*$")) end
      top[#top + 1] = cdata
      pos = cdata_end + 1
    else
      if not tag_start then break end
      local top = stack[#stack]
      local text_segment = text:sub(pos, tag_start - 1)
      if text_segment:find("%S") then top[#top + 1] = xml_unescape(text_segment:match("^%s*(.-)%s*$")) end
      if closing == "/" then
        if #stack < 2 or top.xmlTag ~= name then return nil end
        stack[#stack] = nil
      else
        local el = { xmlTag = name }
        xml_attrs(el, attrs)
        top[#top + 1] = el
        if #stack == 1 then
          if root then return nil end -- a second root element
          root = el
        end
        if selfclose ~= "/" then stack[#stack + 1] = el end
      end
      pos = tag_end + 1
    end
  end
  if #stack ~= 1 or not root then return nil end
  return root
end

-- Element tree -> XML text. A table without xmlTag is a map: { a = 1, b = { c = 2 } } ->
-- <a>1</a><b><c>2</c></b> (keys sorted; an array value repeats the element).
local function xml_encode(value, name)
  if type(value) ~= "table" then
    local text = tostring(value == nil and "" or value):gsub("[&<>\"']", XML_ESC)
    return name and ("<" .. name .. ">" .. text .. "</" .. name .. ">") or text
  end
  if value.xmlTag then
    local tag = tostring(value.xmlTag)
    local parts = { "<" .. tag }
    for _, k in ipairs(sorted_keys(value)) do
      if type(k) == "string" and k ~= "xmlTag" then
        parts[#parts + 1] = " " .. k .. '="' .. tostring(value[k]):gsub("[&<>\"']", XML_ESC) .. '"'
      end
    end
    if #value == 0 then return table.concat(parts) .. "/>" end
    parts[#parts + 1] = ">"
    for _, x in ipairs(value) do parts[#parts + 1] = xml_encode(x) end
    parts[#parts + 1] = "</" .. tag .. ">"
    return table.concat(parts)
  end
  if name and #value > 0 then -- array: the element repeated
    local parts = {}
    for _, x in ipairs(value) do parts[#parts + 1] = xml_encode(x, name) end
    return table.concat(parts)
  end
  local parts = {}
  for _, k in ipairs(sorted_keys(value)) do parts[#parts + 1] = xml_encode(value[k], tostring(k)) end
  local inner = table.concat(parts)
  return name and ("<" .. name .. ">" .. inner .. "</" .. name .. ">") or inner
end

local function content_type_of(headers)
  for _, h in ipairs(headers) do
    if h.name:lower() == "content-type" then return h.value:lower() end
  end
end

local function type_of(ct)
  if not ct or ct == "" then return T_NONE end
  if ct:find("json", 1, true) then return T_JSON end
  if ct:find("xml", 1, true) then return T_XML end
  if ct:find("^text/plain") then return T_TEXT end
  if ct:find("^application/x%-www%-form%-urlencoded") then return T_FORM end
  return T_OTHER
end

-- Body text -> value for the given type (the text itself when it does not parse).
local function parse_body(body_type, text)
  if body_type == T_JSON then
    local value = jdecode(text, 32)
    if value ~= nil then return value end
  elseif body_type == T_XML then
    local value = xml_decode(text)
    if value ~= nil then return value end
  elseif body_type == T_FORM then
    return form_decode(text)
  end
  return text
end

local function serialize_body(body_type, value)
  if value == nil then return "" end
  if type(value) ~= "table" then return tostring(value) end
  if body_type == T_XML then return xml_encode(value) end
  if body_type == T_FORM then return form_encode(value) end
  return json(value) -- JSON, and a table sent as Text / Other / None
end

-- Header text "Name: value\r\n..." -> array of { name = ..., value = ... } in received order
-- (GATE HTTP manual: `for i, v in ipairs(headers) do print(v.name .. ': ' .. v.value) end`).
local function headers_parse(text)
  local headers = {}
  for line in text:gmatch("[^\r\n]+") do
    local header_name, header_value = line:match("^([^:]+):%s*(.-)%s*$")
    if header_name then headers[#headers + 1] = { name = header_name, value = header_value } end
  end
  return headers
end

-- Headers -> lines "Name: value\r\n" (control characters and ':' in names dropped). Accepted: the
-- manual's array of { name = , value = } pairs, its text form "Name: value\r\n...", or a map
-- { Name = value } (sorted).
local function headers_text(header_spec, out, seen)
  if type(header_spec) == "string" then header_spec = headers_parse(header_spec:gsub("\\r\\n", "\n")) end
  if type(header_spec) ~= "table" then return end
  local list = {}
  if #header_spec > 0 then
    for _, header_entry in ipairs(header_spec) do
      if type(header_entry) == "table" and header_entry.name ~= nil then
        list[#list + 1] = { header_entry.name, header_entry.value }
      end
    end
  else
    for _, key in ipairs(sorted_keys(header_spec)) do list[#list + 1] = { key, header_spec[key] } end
  end
  for _, header_entry in ipairs(list) do
    local name = tostring(header_entry[1]):gsub("[%c:]", "")
    if name ~= "" then
      seen[name:lower()] = true
      out[#out + 1] = name .. ": " .. tostring(header_entry[2] == nil and "" or header_entry[2]):gsub("%c", "") .. "\r\n"
    end
  end
end

local function query_text(query)
  if type(query) == "table" then return form_encode(query) end
  if query == nil then return "" end
  return (tostring(query):gsub("^%?", ""))
end

local function num(value, default_value)
  return math.tointeger(tonumber(value)) or default_value
end

-- ---------------------------------------------------------------------------------------------
-- HttpRequest (class 121). Features: 0 Host, 1 Path, 2 QueryStringParams, 3 Method, 4 Timeout [s],
-- 5 RequestType, 6 ResponseType, 7 RequestHeaders, 8 RequestBody, 9 ResponseHeaders,
-- 10 ResponseBody, 11 StatusCode, 12 IsActive. Methods: 0 SendRequest([body]), 1 AbortRequest,
-- 2 Clear. Events: 0 OnRequestSent, 1 OnResponse.
-- ---------------------------------------------------------------------------------------------
local pending = {} -- request id -> HttpRequest

local Req = defclass(121, {})
function Req.init(object)
  local features = object._f
  features[0], features[1], features[3], features[4], features[5], features[6], features[11] = "", "/", "GET", 5, T_TEXT, T_TEXT, 0
end
Req.G[12] = function(object) return object._id and 1 or 0 end
Req.S[11] = function() end -- read-only
Req.S[12] = function() end

local function req_url(object)
  local host = tostring(object._f[0] or ""):gsub("/+$", "")
  if not host:find("^%a[%w+.-]*://") then host = "http://" .. host end
  local path = tostring(object._f[1] or "")
  if path:sub(1, 1) ~= "/" then path = "/" .. path end
  local query = query_text(object._f[2])
  return host .. path .. (query ~= "" and ("?" .. query) or "")
end

Req.X[1] = function(object) -- AbortRequest
  if object._id then
    pending[object._id] = nil
    clu.http_abort(object._id)
    object._id = nil
  end
end

Req.X[0] = function(object, body) -- SendRequest
  Req.X[1](object) -- a request still running is dropped
  local rt, st = num(object._f[5], T_TEXT), num(object._f[6], T_TEXT)
  local lines, seen = {}, {}
  headers_text(object._f[7], lines, seen)
  if CONTENT_TYPE[rt] and not seen["content-type"] then lines[#lines + 1] = "Content-Type: " .. CONTENT_TYPE[rt] .. "\r\n" end
  if CONTENT_TYPE[st] and not seen["accept"] then lines[#lines + 1] = "Accept: " .. CONTENT_TYPE[st] .. "\r\n" end
  if body == nil then body = object._f[8] end
  local method = tostring(object._f[3] or "GET"):upper()
  local id, err = clu.http_send(method, req_url(object), table.concat(lines), serialize_body(rt, body), num(object._f[4], 5))
  if not id then
    clu.log(2, object._name .. ": SendRequest: " .. tostring(err))
    return
  end
  object._id = id
  pending[id] = object
  object:_fire(0) -- OnRequestSent
end

Req.X[2] = function(object) -- Clear
  local features = object._f
  features[7], features[8], features[9], features[10], features[11] = nil, nil, nil, nil, 0
end

local function on_response(id, status, headers, body)
  local object = pending[id]
  pending[id] = nil
  if not object or object._id ~= id then return end
  object._id = nil
  if status < 0 then -- no response (connection, timeout, too large): OnResponse with StatusCode 0
    clu.log(2, object._name .. ": no response")
    object._f[9], object._f[10], object._f[11] = {}, nil, 0
    object:_fire(1) -- OnResponse
    return
  end
  object._f[11] = status
  object._f[9] = headers_parse(headers)
  object._f[10] = parse_body(num(object._f[6], T_TEXT), body)
  object:_fire(1) -- OnResponse
end

-- ---------------------------------------------------------------------------------------------
-- HttpListener (class 120). Features: 0 Path, 1 Method, 2 QueryStringParams, 3 RequestType,
-- 4 RequestHeaders, 5 RequestBody, 6 ResponseType, 7 ResponseHeaders, 8 ResponseBody, 9 StatusCode.
-- Methods: 0 SendResponse([body]), 1 Clear. Events: 0 OnRequest.
-- ---------------------------------------------------------------------------------------------
local Lis = defclass(120, {})
function Lis.init(object)
  local features = object._f
  features[0], features[1], features[3], features[6], features[9] = "", "", T_NONE, T_TEXT, 200
end
Lis.S[1] = function() end -- read-only
Lis.S[3] = function() end

Lis.X[0] = function(object, body) -- SendResponse
  local rid = object._rid
  if not rid then
    clu.log(2, object._name .. ": SendResponse without a request")
    return
  end
  object._rid = nil
  local response_type = num(object._f[6], T_TEXT)
  local lines, seen = {}, {}
  headers_text(object._f[7], lines, seen)
  if CONTENT_TYPE[response_type] and not seen["content-type"] then lines[#lines + 1] = "Content-Type: " .. CONTENT_TYPE[response_type] .. "\r\n" end
  if body == nil then body = object._f[8] end
  if not clu.http_respond(rid, num(object._f[9], 200), table.concat(lines), serialize_body(response_type, body)) then
    clu.log(2, object._name .. ": SendResponse too late (the client got 504)")
  end
end

Lis.X[1] = function(object) -- Clear
  local features = object._f
  features[2], features[4], features[5], features[7], features[8], features[9] = nil, nil, nil, nil, nil, 200
  features[3] = T_NONE
end

local function norm_path(path)
  path = tostring(path or "")
  if path:sub(1, 1) ~= "/" then path = "/" .. path end
  if #path > 1 then path = path:gsub("/+$", "") end
  return path
end

local function find_listener(path)
  for _, object in pairs(OBJECTS) do
    if object._class == 120 and norm_path(object._f[0]) == path then return object end
  end
end

local TEXT_PLAIN = "Content-Type: text/plain\r\n"

local function on_request(rid, method, path, query, headers, body, peer)
  path = norm_path(url_decode(path))
  local object = find_listener(path)
  if not object or not object._ev[0] or #object._ev[0] == 0 then
    clu.http_respond(rid, 404, TEXT_PLAIN, "Not Found")
    return
  end
  local parsed_headers = headers_parse(headers)
  local request_type = type_of(content_type_of(parsed_headers))
  local features = object._f
  features[1], features[2], features[3], features[4] = method, query ~= "" and form_decode(query) or nil, request_type, parsed_headers
  features[5] = body ~= "" and parse_body(request_type, body) or nil
  object._rid, object._peer = rid, peer
  -- OnRequest handlers; one that fails before SendResponse answers 500 (as clu.await timing out
  -- on a GATE, manual 4.5). pcall is yieldable: remote calls in the handler still wait.
  for _, fn in ipairs(object._ev[0]) do
    __spawn(object._name .. " event 0", function()
      local ok, err = pcall(fn)
      if not ok then
        if object._rid == rid then
          object._rid = nil
          clu.http_respond(rid, 500, TEXT_PLAIN, "Internal Server Error")
        end
        error(err, 0)
      end
    end)
  end
end

-- GATE scripts wait for a call on another CLU with clu.await(CluZ->Relay->Switch(0)) (manual 4.5). Here
-- remote calls made in event handlers already wait for their result: clu.await passes it through.
if type(_G.clu) ~= "table" then _G.clu = {} end
if _G.clu.await == nil then _G.clu.await = function(...) return ... end end

local function tick()
  for _ = 1, POLL_PER_TICK do
    local kind, id, status_or_method, headers_or_path, body_or_query, request_headers, request_body, peer = clu.http_poll()
    if not kind then return end
    if kind == "resp" then
      on_response(id, status_or_method, headers_or_path, body_or_query)
    else
      on_request(id, status_or_method, headers_or_path, body_or_query, request_headers, request_body, peer)
    end
  end
end
HOOKS.loop[#HOOKS.loop + 1] = tick

-- for tests / debugging
__http = {
  url_encode = url_encode, url_decode = url_decode, form_encode = form_encode, form_decode = form_decode,
  xml_decode = xml_decode, xml_encode = xml_encode, parse_body = parse_body, serialize_body = serialize_body,
  headers_parse = headers_parse, type_of = type_of,
}
