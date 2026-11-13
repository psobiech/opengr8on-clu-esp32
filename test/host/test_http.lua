-- Host test for components/clu_http/http.lua (GATE HttpRequest / HttpListener) and the local GATE
-- object (GATE:new(5000, ...)) with a Lua stub of the C __clu table. Usage: lua5.3 test_http.lua
local here = arg[0]:match("(.*/)") or "./"
local print = print
local failures = 0
local function check(name, cond)
  print((cond and "ok   " or "FAIL ") .. name)
  if not cond then failures = failures + 1 end
end

-- ---- stub of the C side ------------------------------------------------------
local sent, aborted, responded, inbox, logs = {}, {}, {}, {}, {}
local next_id, send_err, clock = 100, nil, 0
local storage = os.tmpname()
os.remove(storage)

__clu = {
  root = here .. "fixtures",
  storage = storage,
  sn = 221010155,
  gate = true,
  new = function() end,
  get = function() return false end,
  set = function() return false end,
  execute = function() end,
  log = function(_, m) logs[#logs + 1] = m end,
  loop = function() end,
  init = function() end,
  wait = function() end,
  report = function() end,
  millis = function() return clock end,
  remote = function() end,
  http_send = function(method, url, headers, body, timeout)
    if send_err then return nil, send_err end
    next_id = next_id + 1
    sent[#sent + 1] = { id = next_id, method = method, url = url, headers = headers, body = body, timeout = timeout }
    return next_id
  end,
  http_abort = function(id) aborted[#aborted + 1] = id end,
  http_poll = function()
    local m = table.remove(inbox, 1)
    if m then return table.unpack(m, 1, 8) end
  end,
  http_respond = function(id, status, headers, body)
    responded[#responded + 1] = { id = id, status = status, headers = headers, body = body }
    return true
  end,
}

assert(loadfile(here .. "../../components/clu_lua/prelude.lua"))()
assert(loadfile(here .. "../../components/clu_lua/objects.lua"))()
assert(loadfile(here .. "../../components/clu_http/http.lua"))()
__clu = nil
local H = __http

local function run(n)
  for _ = 1, n or 1 do
    clock = clock + 10
    SYSTEM.Loop()
  end
end

-- ---- codecs ------------------------------------------------------------------
check("url encode", H.url_encode("a b&c/é") == "a%20b%26c%2F%C3%A9")
check("url decode", H.url_decode("a+b%26c%2f") == "a b&c/")
check("form encode sorted", H.form_encode({ b = "2 3", a = 1 }) == "a=1&b=2%203")
local f = H.form_decode("x=1&y=a+b&flag")
check("form decode", f.x == "1" and f.y == "a b" and f.flag == "")
check("content type -> type", H.type_of("application/json; charset=utf-8") == 2 and H.type_of("text/xml") == 3
  and H.type_of("text/plain") == 1 and H.type_of("application/x-www-form-urlencoded") == 4
  and H.type_of("image/png") == 5 and H.type_of(nil) == 0)
local j = H.parse_body(2, '{"a":{"b":[1,2,{"c":"d"}]},"n":1.5}')
check("parse JSON", j.a.b[3].c == "d" and j.n == 1.5)
check("parse bad JSON -> text", H.parse_body(2, "{oops") == "{oops")
check("parse text stays text", H.parse_body(1, '{"a":1}') == '{"a":1}')
-- the manual's openweathermap example (2.1.2 B), well-formed
local x = H.xml_decode([[<?xml version="1.0" encoding="UTF-8"?>
<current>
  <city id="2643741" name="City of London">
    <coord lon="-0.09" lat="51.51"/>
    <country>GB</country>
    <sun rise="2015-06-30T03:46:57" set="2015-06-30T20:21:12"/>
  </city>
  <temperature value="72.34" min="66.2" max="79.88" unit="fahrenheit"/>
  <!-- comment -->
  <weather number="800" value="Sky &amp; Clear" icon="01d"/>
</current>]])
check("parse XML as in the manual: resp[1].id, resp[1][2][1], resp[1][2].xmlTag",
  x and x.xmlTag == "current" and x[1].id == "2643741" and x[1][2][1] == "GB" and x[1][2].xmlTag == "country")
check("parse XML: attributes, self-closing elements, entities", x[2].unit == "fahrenheit" and x[1][1].lon == "-0.09"
  and #x[1][1] == 0 and x[3].value == "Sky & Clear" and #x == 3)
local xc = H.xml_decode("<a>x <![CDATA[<b>]]> y<c/></a>")
check("parse XML: CDATA and mixed text kept in order", xc[1] == "x" and xc[2] == "<b>" and xc[3] == "y" and xc[4].xmlTag == "c")
check("parse bad XML -> nil", H.xml_decode("<a><b></a>") == nil and H.xml_decode("plain") == nil
  and H.xml_decode("<a/><b/>") == nil)
check("XML encode element tree", H.xml_encode({ xmlTag = "clu", id = "1", { xmlTag = "t", "2<3" }, { xmlTag = "e" } })
  == '<clu id="1"><t>2&lt;3</t><e/></clu>')
check("XML encode map", H.xml_encode({ r = { a = "1<2", i = { "x", "y" } } }) == "<r><a>1&lt;2</a><i>x</i><i>y</i></r>")
check("serialize table as JSON / form / string as is", H.serialize_body(2, { a = 1 }) == '{"a":1}'
  and H.serialize_body(4, { a = "b c" }) == "a=b%20c" and H.serialize_body(2, "raw") == "raw"
  and H.serialize_body(1, nil) == "")
local hp = H.headers_parse("Content-Type: text/plain\r\nSet-Cookie: a\r\nSet-Cookie: b\r\nbad line\r\n")
check("headers parse: array of {name, value} in order (manual 2.1.3)", #hp == 3 and hp[1].name == "Content-Type"
  and hp[1].value == "text/plain" and hp[3].name == "Set-Cookie" and hp[3].value == "b")

-- ---- OM.LUA of a GATE --------------------------------------------------------
CLU900000006 = GATE:new(5000, 0xC0A8016B, 0xFFFFFF00, 0xC0A8016C, "", "")
HTT0001 = GATE:new(121, "HTT0001")
HTT0002 = GATE:new(120, "HTT0002")
HTT0003 = GATE:new(120, "HTT0003")
check("GATE:new(5000) is the local object (class 0, named CLU<sn>)", CLU900000006._class == 0
  and CLU900000006._name == "CLU221010155" and CLU900000006._ip == 0xC0A8016B
  and CLU900000006._netmask == 0xFFFFFF00 and CLU900000006._gateway == 0xC0A8016C
  and __local_clu() == CLU900000006 and __objects.CLU221010155 == CLU900000006)
check("HttpRequest defaults", HTT0001:get(3) == "GET" and HTT0001:get(4) == 5 and HTT0001:get(5) == 1
  and HTT0001:get(12) == 0 and HTT0001:get(11) == 0)
check("HttpListener defaults", HTT0002:get(9) == 200 and HTT0002:get(6) == 1 and HTT0002:get(3) == 0)

local inits = 0
function OnInit()
  CLU900000006:set(14, 0)
  HTT0001:set(0, "api.example.com/")
  HTT0001:set(1, "v1/items")
  HTT0001:set(2, "\z")
  HTT0002:set(0, "/light")
  HTT0003:set(0, "other/")
  inits = inits + 1
end
CLU900000006:add_event(0, OnInit)
SYSTEM.Init()
check("OnInit fired on the GATE", inits == 1)

-- ---- HttpRequest -------------------------------------------------------------
local events = {}
local function responses()
  local n = 0
  for _, e in ipairs(events) do n = n + (e:find("^resp") and 1 or 0) end
  return n
end
HTT0001:add_event(0, function() events[#events + 1] = "sent" end)
HTT0001:add_event(1, function()
  events[#events + 1] = "resp:" .. HTT0001:get(11)
end)
HTT0001:set(2, { q = "a b" })
HTT0001:set(5, 2)
HTT0001:set(6, 2)
HTT0001:set(7, "X-Key: k1\r\n")
HTT0001:set(3, "post")
HTT0001:execute(0, { temp = 21 })
local r = sent[1]
check("SendRequest: URL with scheme, path, query", r and r.url == "http://api.example.com/v1/items?q=a%20b")
check("SendRequest: method, JSON body, timeout", r.method == "POST" and r.body == '{"temp":21}' and r.timeout == 5)
check("SendRequest: headers", r.headers:find("X-Key: k1\r\n", 1, true) and r.headers:find("Content-Type: application/json\r\n", 1, true)
  and r.headers:find("Accept: application/json\r\n", 1, true))
check("OnRequestSent, IsActive", events[1] == "sent" and HTT0001:get(12) == 1)
inbox[#inbox + 1] = { "resp", r.id, 201, "Content-Type: application/json\r\nX-R: 1\r\n", '{"ok":true,"list":[1,2]}' }
run()
check("OnResponse: status, parsed body, headers", events[2] == "resp:201" and HTT0001:get(10).ok == true
  and HTT0001:get(10).list[2] == 2 and HTT0001:get(9)[2].name == "X-R" and HTT0001:get(9)[2].value == "1"
  and HTT0001:get(12) == 0)

-- a second send while one is running drops the first; its late result is ignored
HTT0001:set(7, { { name = "Content-Type", value = "text/csv" }, { name = "Month", value = "April" } }) -- manual 2.1.3
HTT0001:execute(0, "a;b")
local first = sent[2].id
HTT0001:execute(0)
check("resend aborts the running request", aborted[#aborted] == first and #sent == 3)
check("user Content-Type wins, string body as is", sent[2].headers:find("Content-Type: text/csv\r\nMonth: April\r\n", 1, true)
  and not sent[2].headers:find("Content-Type: application/json", 1, true) and sent[2].body == "a;b")
inbox[#inbox + 1] = { "resp", first, 200, "", "late" }
run()
check("result of an aborted request ignored", responses() == 1 and HTT0001:get(12) == 1)
inbox[#inbox + 1] = { "resp", sent[3].id, -1, "", "" }
run()
check("no response: OnResponse with StatusCode 0, no body, no headers", responses() == 2 and events[#events] == "resp:0"
  and HTT0001:get(11) == 0 and HTT0001:get(10) == nil and #HTT0001:get(9) == 0 and HTT0001:get(12) == 0)
HTT0001:execute(2)
check("Clear", HTT0001:get(10) == nil and HTT0001:get(9) == nil)
send_err = "too many requests in progress"
HTT0001:execute(0)
check("send refused: logged, not active", HTT0001:get(12) == 0 and logs[#logs]:find("too many", 1, true))
send_err = nil
HTT0001:set(0, "https://secure.example.com")
HTT0001:set(1, "/x")
HTT0001:set(2, "?a=1")
HTT0001:execute(0)
check("https host kept, query string as text", sent[#sent].url == "https://secure.example.com/x?a=1")

-- ---- HttpListener ------------------------------------------------------------
local got
HTT0002:add_event(0, function()
  got = { HTT0002:get(1), HTT0002:get(2), HTT0002:get(3), HTT0002:get(4), HTT0002:get(5) }
  HTT0002:set(6, 2)
  HTT0002:set(9, 201)
  HTT0002:execute(0, { on = true })
end)
inbox[#inbox + 1] = { "req", 7, "POST", "/light/", "x=1&y=%20", "Content-Type: application/json\r\nHost: g\r\n",
  '{"level":5}', "192.168.1.5" }
run()
check("OnRequest: method, query, type, headers, body", got and got[1] == "POST" and got[2].x == "1"
  and got[2].y == " " and got[3] == 2 and got[4][2].name == "Host" and got[4][2].value == "g" and got[5].level == 5)
local resp = responded[#responded]
check("SendResponse: status, JSON body, Content-Type", resp.id == 7 and resp.status == 201
  and resp.body == '{"on":true}' and resp.headers == "Content-Type: application/json\r\n")
inbox[#inbox + 1] = { "req", 8, "GET", "/nope", "", "", "", "1.2.3.4" }
inbox[#inbox + 1] = { "req", 9, "GET", "/other", "", "", "", "1.2.3.4" }
run()
check("unknown path -> 404", responded[#responded - 1].id == 8 and responded[#responded - 1].status == 404)
check("listener without OnRequest -> 404", responded[#responded].id == 9 and responded[#responded].status == 404)
-- no query string: QueryStringParams is nil (manual 4.2: `if data == nil then ... Switch`)
local q_seen = "unset"
local LST = GATE:new(120, "HTT0004")
LST:set(0, "/relaycontrol")
LST:add_event(0, function()
  q_seen = LST:get(2)
  LST:set(9, 200)
  LST:set(8, "OK")
  LST:execute(0)
end)
inbox[#inbox + 1] = { "req", 20, "GET", "/relaycontrol", "", "", "", "1.2.3.4" }
run()
check("no query string: QueryStringParams nil, ResponseBody set as a feature", q_seen == nil
  and responded[#responded].id == 20 and responded[#responded].body == "OK")
-- a handler failing before SendResponse: 500 (manual 4.5, clu.await timeout)
local FAIL = GATE:new(120, "HTT0005")
FAIL:set(0, "/fail")
FAIL:add_event(0, function() error("remote timeout") end)
inbox[#inbox + 1] = { "req", 21, "GET", "/fail", "", "", "", "1.2.3.4" }
run()
check("handler error before SendResponse -> 500", responded[#responded].id == 21 and responded[#responded].status == 500)
check("clu.await passes the result through", clu.await(7) == 7)
-- response headers as the manual's {name, value} pairs
local HDR = GATE:new(120, "HTT0006")
HDR:set(0, "/h")
HDR:add_event(0, function()
  HDR:set(7, { { name = "Month", value = "April" }, { name = "Year", value = "2023" } })
  HDR:execute(0, "x")
end)
inbox[#inbox + 1] = { "req", 22, "GET", "/h", "", "", "", "1.2.3.4" }
run()
check("ResponseHeaders as {name, value} pairs, order kept", responded[#responded].headers
  == "Month: April\r\nYear: 2023\r\nContent-Type: text/plain\r\n")

-- handler answering later (e.g. after a remote call): the request waits for SendResponse
HTT0003:add_event(0, function() end)
local n_before = #responded
inbox[#inbox + 1] = { "req", 10, "GET", "/other", "", "", "", "1.2.3.4" }
run()
check("no SendResponse yet: nothing sent", #responded == n_before)
HTT0003:set(7, "X-A: 1\r\n")
HTT0003:set(8, "hello")
HTT0003:execute(0)
check("SendResponse later: text, extra headers", responded[#responded].id == 10 and responded[#responded].body == "hello"
  and responded[#responded].headers == "X-A: 1\r\nContent-Type: text/plain\r\n")
HTT0003:execute(0)
check("second SendResponse: nothing to answer", responded[#responded].id == 10 and logs[#logs]:find("without a request", 1, true))
HTT0003:execute(1)
check("listener Clear", HTT0003:get(8) == nil and HTT0003:get(9) == 200)
check("read-only listener features", (function() HTT0002:set(1, "X"); return HTT0002:get(1) == "POST" end)())

print(failures == 0 and "\nPASSED (0 failures)" or ("\nFAILED (" .. failures .. " failures)"))
os.remove(storage)
os.exit(failures == 0 and 0 or 1)
