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
local C = __clu
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
local function url_encode(s)
  return (tostring(s):gsub("[^%w%-%._~]", function(c) return string.format("%%%02X", c:byte()) end))
end

local function url_decode(s)
  s = s:gsub("%+", " ")
  return (s:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end))
end

local function sorted_keys(t)
  local keys = {}
  for k in pairs(t) do keys[#keys + 1] = k end
  table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
  return keys
end

local function form_encode(t)
  local parts = {}
  for _, k in ipairs(sorted_keys(t)) do
    parts[#parts + 1] = url_encode(k) .. "=" .. url_encode(t[k])
  end
  return table.concat(parts, "&")
end

local function form_decode(s)
  local t = {}
  for part in s:gmatch("[^&]+") do
    local k, v = part:match("^([^=]*)=(.*)$")
    if k then t[url_decode(k)] = url_decode(v) else t[url_decode(part)] = "" end
  end
  return t
end

local XML_ESC = { ["&"] = "&amp;", ["<"] = "&lt;", [">"] = "&gt;", ['"'] = "&quot;", ["'"] = "&apos;" }
local XML_UNESC = { amp = "&", lt = "<", gt = ">", quot = '"', apos = "'" }

local function xml_unescape(s)
  return (s:gsub("&(#?x?)(%w+);", function(p, n)
    if p == "" then return XML_UNESC[n] end
    local c = tonumber(n, p == "#x" and 16 or 10)
    return c and utf8.char(c) or nil
  end))
end

local function xml_attrs(el, text)
  for k, q, v in text:gmatch("([%a_][%w_:%.%-]*)%s*=%s*([\"'])(.-)%2") do
    el[k] = xml_unescape(v)
  end
end

-- XML text -> element tree (GATE HTTP manual): an element is a table with its tag name in
-- `xmlTag`, its attributes as fields and its children (elements, text) at [1], [2], ...; e.g.
-- <current><city id="7"><country>GB</country></city></current> -> resp[1].id == "7",
-- resp[1][1][1] == "GB", resp[1][1].xmlTag == "country". Returns the root element; whitespace-only
-- text is dropped. nil on malformed input.
local function xml_decode(s)
  s = s:gsub("<%?.-%?>", ""):gsub("<!%-%-.-%-%->", ""):gsub("<!DOCTYPE.->", "")
  local stack, root = { { } }, nil
  local pos = 1
  while true do
    local a, b, close, name, attrs, selfclose = s:find("<(/?)([%a_][%w_:%.%-]*)([^>]-)(/?)>", pos)
    local cdata_a, cdata_b, cdata = s:find("<!%[CDATA%[(.-)%]%]>", pos)
    if cdata_a and (not a or cdata_a < a) then -- CDATA before the next tag: literal text
      local top = stack[#stack]
      local text = s:sub(pos, cdata_a - 1)
      if text:find("%S") then top[#top + 1] = xml_unescape(text:match("^%s*(.-)%s*$")) end
      top[#top + 1] = cdata
      pos = cdata_b + 1
    else
      if not a then break end
      local top = stack[#stack]
      local text = s:sub(pos, a - 1)
      if text:find("%S") then top[#top + 1] = xml_unescape(text:match("^%s*(.-)%s*$")) end
      if close == "/" then
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
      pos = b + 1
    end
  end
  if #stack ~= 1 or not root then return nil end
  return root
end

-- Element tree -> XML text. A table without xmlTag is a map: { a = 1, b = { c = 2 } } ->
-- <a>1</a><b><c>2</c></b> (keys sorted; an array value repeats the element).
local function xml_encode(v, name)
  if type(v) ~= "table" then
    local text = tostring(v == nil and "" or v):gsub("[&<>\"']", XML_ESC)
    return name and ("<" .. name .. ">" .. text .. "</" .. name .. ">") or text
  end
  if v.xmlTag then
    local tag = tostring(v.xmlTag)
    local parts = { "<" .. tag }
    for _, k in ipairs(sorted_keys(v)) do
      if type(k) == "string" and k ~= "xmlTag" then
        parts[#parts + 1] = " " .. k .. '="' .. tostring(v[k]):gsub("[&<>\"']", XML_ESC) .. '"'
      end
    end
    if #v == 0 then return table.concat(parts) .. "/>" end
    parts[#parts + 1] = ">"
    for _, x in ipairs(v) do parts[#parts + 1] = xml_encode(x) end
    parts[#parts + 1] = "</" .. tag .. ">"
    return table.concat(parts)
  end
  if name and #v > 0 then -- array: the element repeated
    local parts = {}
    for _, x in ipairs(v) do parts[#parts + 1] = xml_encode(x, name) end
    return table.concat(parts)
  end
  local parts = {}
  for _, k in ipairs(sorted_keys(v)) do parts[#parts + 1] = xml_encode(v[k], tostring(k)) end
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
local function parse_body(t, text)
  if t == T_JSON then
    local v = jdecode(text, 32)
    if v ~= nil then return v end
  elseif t == T_XML then
    local v = xml_decode(text)
    if v ~= nil then return v end
  elseif t == T_FORM then
    return form_decode(text)
  end
  return text
end

local function serialize_body(t, v)
  if v == nil then return "" end
  if type(v) ~= "table" then return tostring(v) end
  if t == T_XML then return xml_encode(v) end
  if t == T_FORM then return form_encode(v) end
  return json(v) -- JSON, and a table sent as Text / Other / None
end

-- Header text "Name: value\r\n..." -> array of { name = ..., value = ... } in received order
-- (GATE HTTP manual: `for i, v in ipairs(headers) do print(v.name .. ': ' .. v.value) end`).
local function headers_parse(text)
  local h = {}
  for line in text:gmatch("[^\r\n]+") do
    local k, v = line:match("^([^:]+):%s*(.-)%s*$")
    if k then h[#h + 1] = { name = k, value = v } end
  end
  return h
end

-- Headers -> lines "Name: value\r\n" (control characters and ':' in names dropped). Accepted: the
-- manual's array of { name = , value = } pairs, its text form "Name: value\r\n...", or a map
-- { Name = value } (sorted).
local function headers_text(h, out, seen)
  if type(h) == "string" then h = headers_parse(h:gsub("\\r\\n", "\n")) end
  if type(h) ~= "table" then return end
  local list = {}
  if #h > 0 then
    for _, p in ipairs(h) do
      if type(p) == "table" and p.name ~= nil then list[#list + 1] = { p.name, p.value } end
    end
  else
    for _, k in ipairs(sorted_keys(h)) do list[#list + 1] = { k, h[k] } end
  end
  for _, p in ipairs(list) do
    local name = tostring(p[1]):gsub("[%c:]", "")
    if name ~= "" then
      seen[name:lower()] = true
      out[#out + 1] = name .. ": " .. tostring(p[2] == nil and "" or p[2]):gsub("%c", "") .. "\r\n"
    end
  end
end

local function query_text(q)
  if type(q) == "table" then return form_encode(q) end
  if q == nil then return "" end
  return (tostring(q):gsub("^%?", ""))
end

local function num(v, def)
  return math.tointeger(tonumber(v)) or def
end

-- ---------------------------------------------------------------------------------------------
-- HttpRequest (class 121). Features: 0 Host, 1 Path, 2 QueryStringParams, 3 Method, 4 Timeout [s],
-- 5 RequestType, 6 ResponseType, 7 RequestHeaders, 8 RequestBody, 9 ResponseHeaders,
-- 10 ResponseBody, 11 StatusCode, 12 IsActive. Methods: 0 SendRequest([body]), 1 AbortRequest,
-- 2 Clear. Events: 0 OnRequestSent, 1 OnResponse.
-- ---------------------------------------------------------------------------------------------
local pending = {} -- request id -> HttpRequest

local Req = defclass(121, {})
function Req.init(o)
  local f = o._f
  f[0], f[1], f[3], f[4], f[5], f[6], f[11] = "", "/", "GET", 5, T_TEXT, T_TEXT, 0
end
Req.G[12] = function(o) return o._id and 1 or 0 end
Req.S[11] = function() end -- read-only
Req.S[12] = function() end

local function req_url(o)
  local host = tostring(o._f[0] or ""):gsub("/+$", "")
  if not host:find("^%a[%w+.-]*://") then host = "http://" .. host end
  local path = tostring(o._f[1] or "")
  if path:sub(1, 1) ~= "/" then path = "/" .. path end
  local q = query_text(o._f[2])
  return host .. path .. (q ~= "" and ("?" .. q) or "")
end

Req.X[1] = function(o) -- AbortRequest
  if o._id then
    pending[o._id] = nil
    C.http_abort(o._id)
    o._id = nil
  end
end

Req.X[0] = function(o, body) -- SendRequest
  Req.X[1](o) -- a request still running is dropped
  local rt, st = num(o._f[5], T_TEXT), num(o._f[6], T_TEXT)
  local lines, seen = {}, {}
  headers_text(o._f[7], lines, seen)
  if CONTENT_TYPE[rt] and not seen["content-type"] then lines[#lines + 1] = "Content-Type: " .. CONTENT_TYPE[rt] .. "\r\n" end
  if CONTENT_TYPE[st] and not seen["accept"] then lines[#lines + 1] = "Accept: " .. CONTENT_TYPE[st] .. "\r\n" end
  if body == nil then body = o._f[8] end
  local method = tostring(o._f[3] or "GET"):upper()
  local id, err = C.http_send(method, req_url(o), table.concat(lines), serialize_body(rt, body), num(o._f[4], 5))
  if not id then
    C.log(2, o._name .. ": SendRequest: " .. tostring(err))
    return
  end
  o._id = id
  pending[id] = o
  o:_fire(0) -- OnRequestSent
end

Req.X[2] = function(o) -- Clear
  local f = o._f
  f[7], f[8], f[9], f[10], f[11] = nil, nil, nil, nil, 0
end

local function on_response(id, status, headers, body)
  local o = pending[id]
  pending[id] = nil
  if not o or o._id ~= id then return end
  o._id = nil
  if status < 0 then -- no response (connection, timeout, too large): OnResponse with StatusCode 0
    C.log(2, o._name .. ": no response")
    o._f[9], o._f[10], o._f[11] = {}, nil, 0
    o:_fire(1) -- OnResponse
    return
  end
  o._f[11] = status
  o._f[9] = headers_parse(headers)
  o._f[10] = parse_body(num(o._f[6], T_TEXT), body)
  o:_fire(1) -- OnResponse
end

-- ---------------------------------------------------------------------------------------------
-- HttpListener (class 120). Features: 0 Path, 1 Method, 2 QueryStringParams, 3 RequestType,
-- 4 RequestHeaders, 5 RequestBody, 6 ResponseType, 7 ResponseHeaders, 8 ResponseBody, 9 StatusCode.
-- Methods: 0 SendResponse([body]), 1 Clear. Events: 0 OnRequest.
-- ---------------------------------------------------------------------------------------------
local Lis = defclass(120, {})
function Lis.init(o)
  local f = o._f
  f[0], f[1], f[3], f[6], f[9] = "", "", T_NONE, T_TEXT, 200
end
Lis.S[1] = function() end -- read-only
Lis.S[3] = function() end

Lis.X[0] = function(o, body) -- SendResponse
  local rid = o._rid
  if not rid then
    C.log(2, o._name .. ": SendResponse without a request")
    return
  end
  o._rid = nil
  local t = num(o._f[6], T_TEXT)
  local lines, seen = {}, {}
  headers_text(o._f[7], lines, seen)
  if CONTENT_TYPE[t] and not seen["content-type"] then lines[#lines + 1] = "Content-Type: " .. CONTENT_TYPE[t] .. "\r\n" end
  if body == nil then body = o._f[8] end
  if not C.http_respond(rid, num(o._f[9], 200), table.concat(lines), serialize_body(t, body)) then
    C.log(2, o._name .. ": SendResponse too late (the client got 504)")
  end
end

Lis.X[1] = function(o) -- Clear
  local f = o._f
  f[2], f[4], f[5], f[7], f[8], f[9] = nil, nil, nil, nil, nil, 200
  f[3] = T_NONE
end

local function norm_path(p)
  p = tostring(p or "")
  if p:sub(1, 1) ~= "/" then p = "/" .. p end
  if #p > 1 then p = p:gsub("/+$", "") end
  return p
end

local function find_listener(path)
  for _, o in pairs(OBJECTS) do
    if o._class == 120 and norm_path(o._f[0]) == path then return o end
  end
end

local TEXT_PLAIN = "Content-Type: text/plain\r\n"

local function on_request(rid, method, path, query, headers, body, peer)
  path = norm_path(url_decode(path))
  local o = find_listener(path)
  if not o or not o._ev[0] or #o._ev[0] == 0 then
    C.http_respond(rid, 404, TEXT_PLAIN, "Not Found")
    return
  end
  local h = headers_parse(headers)
  local t = type_of(content_type_of(h))
  local f = o._f
  f[1], f[2], f[3], f[4] = method, query ~= "" and form_decode(query) or nil, t, h
  f[5] = body ~= "" and parse_body(t, body) or nil
  o._rid, o._peer = rid, peer
  -- OnRequest handlers; one that fails before SendResponse answers 500 (as clu.await timing out
  -- on a GATE, manual 4.5). pcall is yieldable: remote calls in the handler still wait.
  for _, fn in ipairs(o._ev[0]) do
    __spawn(o._name .. " event 0", function()
      local ok, err = pcall(fn)
      if not ok then
        if o._rid == rid then
          o._rid = nil
          C.http_respond(rid, 500, TEXT_PLAIN, "Internal Server Error")
        end
        error(err, 0)
      end
    end)
  end
end

-- GATE scripts wait for a call on another CLU with clu.await(CluZ->Relay->Switch(0)) (manual 4.5). Here
-- remote calls made in event handlers already wait for their result: clu.await passes it through.
if type(clu) ~= "table" then clu = {} end
if clu.await == nil then clu.await = function(...) return ... end end

local function tick()
  for _ = 1, POLL_PER_TICK do
    local kind, id, a, b, c, d, e, g = C.http_poll()
    if not kind then return end
    if kind == "resp" then
      on_response(id, a, b, c)
    else
      on_request(id, a, b, c, d, e, g)
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
