# GATE HTTP

Przy `Device type` (`CLU_DEVICE`) = `GATE HTTP` urządzenie przedstawia się Object Managerowi jako
**Grenton GATE HTTP** zamiast CLU_ZWAVE_2: hwType 0x12, hwVer 2, fwType 3, domyślnie firmware
`1.4.2-2346` z fwApiVer 0x578, więc OM wybiera interfejs
`clu_GATE_HTTP_ft00000003_fv00000578_ht00000012_hv00000002.xml`. Opcja działa na każdej
obsługiwanej płytce (z PSRAM, zob. [Wymagania pamięci](DEVICES.md#wymagania-pamięci)).

Zachowanie obiektów według
[instrukcji GATE HTTP](https://storage.grenton.com/docs/manuals/beta/GATE_HTTP_User_Manual_PL.pdf)
(firmware 1.4.2-2346): skrypty z instrukcji działają bez zmian.

Gotowy obraz: preset `gate-http` (ESP32-S3, 16 MB flash / 8 MB PSRAM, WiFi), w wydaniach
`opengr8on-gate-http-esp32s3-<wersja>.bin` / `…-ota.bin`.
Dla ESP32-S3 N8R8 (8 MB flash / 8 MB octal PSRAM): preset `gate-http-N8R8`, obrazy
`opengr8on-gate-http-esp32s3-<wersja>-N8R8.bin` / `…-<wersja>-ota-N8R8.bin`.
Wariant N8R8 ma dwa sloty OTA po 3 MB i FAT 1920 KB. Zmiana układu partycji wymaga pełnej
instalacji przez USB z kasowaniem danych, nie aktualizacji OTA.

GATE nie ma magistrali: przy tym typie `CLU_BUS` ma do wyboru tylko `CLU_BUS_NONE` (UART i piny RS485
nieużywane), w OM nie ma modułów TF-Bus.

## Konfiguracja (menuconfig)

`OpenGr8on ESP32 CLU` → `Device type` = `GATE HTTP`, podmenu
`GATE HTTP objects (HttpRequest / HttpListener)`:

| Opcja | Domyślnie | Opis |
|---|---|---|
| `CLU_HTTP` | y przy GATE | obiekty HttpRequest i HttpListener |
| `CLU_HTTP_PORT` | 80 | port TCP listenera (jak w prawdziwym GATE); inny niż port OTA |
| `CLU_HTTP_MAX_BODY` | 16384 | największa treść żądania / odpowiedzi w bajtach |
| `CLU_HTTP_LISTENER_TIMEOUT_MS` | 10000 | ile żądanie czeka na `SendResponse`, potem 504 |

## MAIN.LUA

MAIN.LUA prawdziwego GATE (ALARM, MODBUS) kończy się na `SYSTEM.Init()` (bez pętli
`repeat SYSTEM.Loop() until …` jak na CLU), a `checkAlive()` tylko wypisuje `true`. Po powrocie
skryptu firmware sam wywołuje `SYSTEM.Loop()` (timery, HTTP, raporty do OM); w logu
`MAIN.LUA done, the firmware runs SYSTEM.Loop`. Na zapytanie `checkAlive()` GATE odpowiada sam,
numerem seryjnym w hex (jak prawdziwe GATE; `emergency` w trybie awaryjnym), bez wywoływania
funkcji ze skryptu.

## Obiekt GATE w OM.LUA

OM deklaruje GATE inaczej niż CLU, bez nazwy, z maską i bramą sieci projektu:

```lua
CLU900000006 = GATE:new(5000, 0xC0A8016B, 0xFFFFFF00, 0xC0A8016C, "", "")
```

Firmware traktuje to jako lokalne urządzenie (klasa 0, nazwa `CLU<seryjny>`). OnInit dodawany jest
przez `add_event(0, OnInit)`, jak na CLU. Cechy według interfejsu GATE:

| Indeks | Cecha | Obsługa |
|---|---|---|
| 0 | Uptime | sekundy od startu (od miękkiego resetu też) |
| 1 | ClientReportInterval | zapamiętywana |
| 2 / 3 | PrimaryDNS / SecondaryDNS | serwery DNS obu interfejsów |
| 5 / 6 | Date / Time | `RRRR-MM-DD` / `gg:mm:ss` czasu lokalnego |
| 13 / 15 | LocalTime / UnixTime | znacznik czasu |
| 14 | TimeZone | strefa czasowa (to samo wyliczenie co na CLU, 0–22), zapisywana w NVS |
| 17 | FirmwareVersion | `1.4.2-2346` |
| 18 / 19 | UseCloud / CloudConnection | połączenie MQTT, jak na CLU ([MQTT](../README.md#mqtt--home-assistant)) |
| 20 | NTPTimeout | zapamiętywana |
| 21 | UseNTP | 0 zatrzymuje SNTP (niezapisywane: OnInit ustawia ją przy każdym starcie); `UseCloud` = 1 włącza ją też (jak w instrukcji) |
| 23 | TelnetLogLevel | zapamiętywana (log telnet zawsze pełny) |
| 25 | OverloadDetection | zapamiętywana |
| 26 | ResetReason | 0 włączenie zasilania, 1 reset zewnętrzny, 2 restart programowy / przeładowanie konfiguracji (miękki reset), 3 wyjątek / watchdog / brownout |

Metoda `SetDateTime` (2) jak na CLU. Serwera NTP GATE nie ustawia (brak cechy): domyślny
`pool.ntp.org`, albo zapisany wcześniej w NVS.

Obiekty: Timer (klasa 6, wersja 2 GATE ma podzbiór cech wersji 1), HttpRequest (121),
HttpListener (120). CoolMasterNet, Sonos, MusicCast, HEOS, DenonMarantzAVR nie są obsługiwane.

## Typy treści

`RequestType` / `ResponseType`: 0 None, 1 Text, 2 JSON, 3 XML, 4 FormData, 5 Other.

- Odebrana treść JSON, XML i FormData staje się tablicą Lua; jeśli się nie da sparsować, zostaje
  tekstem. Text, Other i None zostają tekstem. Pusta treść żądania do listenera: `nil`.
- JSON: obiekty i tablice jak w danych, tablice od 1 (`resp.weather[1].description`).
- XML: drzewo elementów jak w instrukcji. Element to tablica z nazwą w `xmlTag`, atrybutami jako
  polami (napisy) i dziećmi (elementy, tekst) pod `[1]`, `[2]`, …; wynik to element główny:

  ```lua
  -- <current><city id="2643741"><coord lon="-0.09"/><country>GB</country></city></current>
  resp[1].id          -- "2643741"
  resp[1][2][1]       -- "GB"
  resp[1][2].xmlTag   -- "country"
  ```
- Wysyłana tablica: JSON; XML z drzewa elementów (`xmlTag` …) albo z mapy
  (`{ a = 1 }` → `<a>1</a>`, klucze alfabetycznie); `a=1&b=2` dla FormData. Przy Text / Other /
  None tablica idzie jako JSON. Napis jest wysyłany bez zmian.
- Nagłówki (`RequestHeaders`, `ResponseHeaders`): tablica par jak w instrukcji
  (`{ { name = 'Month', value = 'April' }, … }`), napis (`'Month: April\r\n' .. …`) albo mapa
  `{ Month = 'April' }`. Odebrane nagłówki to tablica par w kolejności z odpowiedzi:
  `for i, v in ipairs(h) do print(v.name .. ': ' .. v.value) end`. `Content-Type` / `Accept`
  wynikają z typów, chyba że skrypt poda własne.
- `QueryStringParams`: tablica albo napis (`q=London&APPID=…`); w listenerze tablica parametrów,
  `nil` gdy adres nie ma zapytania.

## HttpRequest (121)

| Indeks | Cecha | Uwagi |
|---|---|---|
| 0 | Host | `http://…`, `https://…` albo sam host (wtedy `http://`), z portem |
| 1 | Path | `/` dodawany, jeśli brak |
| 2 | QueryStringParams | tablica albo napis |
| 3 | Method | GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS |
| 4 | Timeout | 1–255 s |
| 5 / 6 | RequestType / ResponseType | domyślnie 1 (Text) |
| 7 / 8 | RequestHeaders / RequestBody | |
| 9 / 10 | ResponseHeaders / ResponseBody | po odpowiedzi |
| 11 | StatusCode | kod HTTP; 0 gdy nie było odpowiedzi |
| 12 | IsActive | 1 od `SendRequest` do odpowiedzi |

Metody: `SendRequest([treść])` (0; argument zastępuje `RequestBody`), `AbortRequest` (1),
`Clear` (2). Zdarzenia: `OnRequestSent` (0) zaraz po przyjęciu żądania, `OnResponse` (1) po
odpowiedzi.

- Żądania wykonuje osobne zadanie po kolei (najwyżej 4 w kolejce); skrypt nie czeka.
- HTTPS sprawdzane względem wbudowanego zestawu certyfikatów ESP-IDF; przekierowania wykonywane.
- `SendRequest` w trakcie poprzedniego żądania tego obiektu porzuca poprzednie (jego wynik jest
  ignorowany).
- Brak odpowiedzi (błąd połączenia, przekroczony czas, odpowiedź większa niż
  `CLU_HTTP_MAX_BODY`): `OnResponse` z StatusCode 0, bez `ResponseBody` (nil) i z pustą tablicą
  `ResponseHeaders`; ostrzeżenie w logu. Sprawdzaj `StatusCode` w obsłudze zdarzenia.

## HttpListener (120)

| Indeks | Cecha | Uwagi |
|---|---|---|
| 0 | Path | ścieżka, na którą odpowiada ten listener (np. `/light`) |
| 1 | Method | metoda odebranego żądania |
| 2 | QueryStringParams | tablica parametrów |
| 3 | RequestType | z `Content-Type` żądania |
| 4 / 5 | RequestHeaders / RequestBody | |
| 6 | ResponseType | domyślnie 1 (Text) |
| 7 / 8 | ResponseHeaders / ResponseBody | |
| 9 | StatusCode | domyślnie 200 |

Metody: `SendResponse([treść])` (0; argument zastępuje `ResponseBody`), `Clear` (1). Zdarzenie:
`OnRequest` (0).

- Wszystkie listenery dzielą jeden serwer na `CLU_HTTP_PORT`. Żądanie trafia do listenera, którego
  `Path` jest równa ścieżce żądania (dokładnie, bez zapytania; ukośnik na końcu i brak ukośnika na
  początku `Path` są pomijane). Brak takiego listenera albo brak obsługi `OnRequest`: 404.
- Żądania są obsługiwane po kolei; odpowiedź może przyjść później (np. po wywołaniu innej CLU w
  obsłudze zdarzenia). Bez `SendResponse` w `CLU_HTTP_LISTENER_TIMEOUT_MS`: 504. Obsługa
  `OnRequest`, która zakończy się błędem przed `SendResponse`: 500.
- `clu.await(CluZ->Relay->Switch(0))` z instrukcji działa: wywołania innej CLU w obsłudze zdarzenia
  i tak czekają na wynik, `clu.await` go przekazuje.
- HTTP/1.1 bez keep-alive (`Connection: close`). Nieobsługiwane: `Transfer-Encoding: chunked`
  w żądaniu (411), treść ponad `CLU_HTTP_MAX_BODY` (413), nagłówki ponad 4 KB (431).
- **Brak uwierzytelniania i szyfrowania**: każdy w sieci może wywołać listenery, jak na
  prawdziwym GATE. Sprawdzaj w skrypcie np. własny nagłówek z tokenem i nie wystawiaj portu poza
  sieć lokalną.

## Przykład

```lua
-- OnInit
HTT0001:set(0, "https://api.example.com")
HTT0001:set(1, "/v1/weather")
HTT0001:set(2, { city = "Warszawa" })
HTT0001:set(6, 2)                       -- odpowiedź JSON
HTT0002:set(0, "/light")

-- HTT0001 OnResponse
if HTT0001:get(11) == 200 then
  temperatura = HTT0001:get(10).current.temp
end

-- HTT0002 OnRequest: GET /light?on=1
local q = HTT0002:get(2) or {}
CLU900000001:execute(0, "DOU0001:set(0, " .. (q.on == "1" and 1 or 0) .. ")")
HTT0002:set(6, 2)
HTT0002:execute(0, { ok = true })       -- {"ok":true}
```

Niezweryfikowane na prawdziwym GATE (instrukcja tego nie opisuje): `OnResponse` ze StatusCode 0 przy
błędzie połączenia (przyjęte), czy `Path` dopasowuje się też po prefiksie, czy liczby w atrybutach
XML są napisami.

---

# GATE HTTP

With `Device type` (`CLU_DEVICE`) = `GATE HTTP`, the device identifies itself to Object Manager as
**Grenton GATE HTTP** instead of CLU_ZWAVE_2: hwType 0x12, hwVer 2, fwType 3, default firmware
`1.4.2-2346` with fwApiVer 0x578, so OM selects the
`clu_GATE_HTTP_ft00000003_fv00000578_ht00000012_hv00000002.xml` interface. Works on any supported
board (with PSRAM, see [Memory requirements](DEVICES.md#memory-requirements)).

Object behavior follows the
[GATE HTTP manual (PL)](https://storage.grenton.com/docs/manuals/beta/GATE_HTTP_User_Manual_PL.pdf)
(firmware 1.4.2-2346): scripts from the manual work without changes.

Prebuilt image: `gate-http` preset (ESP32-S3, 16 MB flash / 8 MB PSRAM, WiFi), in releases as
`opengr8on-gate-http-esp32s3-<version>.bin` / `…-ota.bin`.
For ESP32-S3 N8R8 (8 MB flash / 8 MB octal PSRAM): `gate-http-N8R8` preset, images
`opengr8on-gate-http-esp32s3-<version>-N8R8.bin` / `…-<version>-ota-N8R8.bin`.
The N8R8 variant has two 3 MB OTA slots and 1920 KB FAT. Changing the partition layout requires a
full USB flash with erase, not an OTA update.

GATE has no bus: with this type, `CLU_BUS` only offers `CLU_BUS_NONE` (UART and RS485 pins
unused), and OM shows no TF-Bus modules.

## Configuration (menuconfig)

`OpenGr8on ESP32 CLU` → `Device type` = `GATE HTTP`, submenu
`GATE HTTP objects (HttpRequest / HttpListener)`:

| Option | Default | Description |
|---|---|---|
| `CLU_HTTP` | y for GATE | HttpRequest and HttpListener objects |
| `CLU_HTTP_PORT` | 80 | listener TCP port (same as real GATE); must differ from OTA port |
| `CLU_HTTP_MAX_BODY` | 16384 | maximum request / response body in bytes |
| `CLU_HTTP_LISTENER_TIMEOUT_MS` | 10000 | how long a request waits for `SendResponse` before 504 |

## MAIN.LUA

On a real GATE (ALARM, MODBUS), MAIN.LUA ends with `SYSTEM.Init()` (no `repeat SYSTEM.Loop() until …`
loop as on CLU), and `checkAlive()` only prints `true`. After the script returns, the firmware calls
`SYSTEM.Loop()` itself (timers, HTTP, reports to OM); the log shows
`MAIN.LUA done, the firmware runs SYSTEM.Loop`. GATE answers `checkAlive()` queries directly with its
serial number in hex (like a real GATE; `emergency` in emergency mode), without calling the script
function.

## GATE object in OM.LUA

OM declares GATE differently than CLU, without a name, with the project network mask and gateway:

```lua
CLU900000006 = GATE:new(5000, 0xC0A8016B, 0xFFFFFF00, 0xC0A8016C, "", "")
```

The firmware treats it as the local device (class 0, name `CLU<serial>`). OnInit is added via
`add_event(0, OnInit)`, same as on CLU. Properties per the GATE interface:

| Index | Property | Handling |
|---|---|---|
| 0 | Uptime | seconds since boot (also from soft reset) |
| 1 | ClientReportInterval | stored |
| 2 / 3 | PrimaryDNS / SecondaryDNS | DNS servers for both interfaces |
| 5 / 6 | Date / Time | `YYYY-MM-DD` / `hh:mm:ss` local time |
| 13 / 15 | LocalTime / UnixTime | timestamp |
| 14 | TimeZone | time zone (same enum as CLU, 0–22), saved in NVS |
| 17 | FirmwareVersion | `1.4.2-2346` |
| 18 / 19 | UseCloud / CloudConnection | MQTT connection, same as CLU ([MQTT](../README.md#mqtt--home-assistant)) |
| 20 | NTPTimeout | stored |
| 21 | UseNTP | 0 stops SNTP (not persisted: OnInit sets it on every boot); `UseCloud` = 1 also enables it (as in the manual) |
| 23 | TelnetLogLevel | stored (telnet log is always full) |
| 25 | OverloadDetection | stored |
| 26 | ResetReason | 0 power-on, 1 external reset, 2 software restart / config reload (soft reset), 3 exception / watchdog / brownout |

`SetDateTime` method (2) works the same as on CLU. GATE does not configure an NTP server (no
property): defaults to `pool.ntp.org`, or whatever was previously saved in NVS.

Objects: Timer (class 6, GATE v2 has a subset of v1 features), HttpRequest (121),
HttpListener (120). CoolMasterNet, Sonos, MusicCast, HEOS, DenonMarantzAVR are not supported.

## Content types

`RequestType` / `ResponseType`: 0 None, 1 Text, 2 JSON, 3 XML, 4 FormData, 5 Other.

- Received JSON, XML, and FormData bodies become a Lua table; if parsing fails, they remain a
  string. Text, Other, and None remain a string. Empty listener request body: `nil`.
- JSON: objects and arrays match the data, 1-based arrays (`resp.weather[1].description`).
- XML: element tree as in the manual. An element is a table with its name in `xmlTag`, attributes as
  fields (strings), and children (elements, text) at `[1]`, `[2]`, …; the result wraps the root
  element:

  ```lua
  -- <current><city id="2643741"><coord lon="-0.09"/><country>GB</country></city></current>
  resp[1].id          -- "2643741"
  resp[1][2][1]       -- "GB"
  resp[1][2].xmlTag   -- "country"
  ```
- Outgoing table: JSON; XML from an element tree (`xmlTag` …) or from a map
  (`{ a = 1 }` → `<a>1</a>`, keys sorted alphabetically); `a=1&b=2` for FormData. For Text / Other /
  None, a table is sent as JSON. A string is sent as-is.
- Headers (`RequestHeaders`, `ResponseHeaders`): array of pairs as in the manual
  (`{ { name = 'Month', value = 'April' }, … }`), string (`'Month: April\r\n' .. …`), or map
  `{ Month = 'April' }`. Received headers are an array of pairs in response order:
  `for i, v in ipairs(h) do print(v.name .. ': ' .. v.value) end`. `Content-Type` / `Accept`
  are derived from the types unless provided by the script.
- `QueryStringParams`: table or string (`q=London&APPID=…`); in the listener a parameter table,
  `nil` when the URL has no query string.

## HttpRequest (121)

| Index | Property | Notes |
|---|---|---|
| 0 | Host | `http://…`, `https://…`, or bare host (defaults to `http://`), with optional port |
| 1 | Path | `/` prepended if missing |
| 2 | QueryStringParams | table or string |
| 3 | Method | GET, POST, PUT, PATCH, DELETE, HEAD, OPTIONS |
| 4 | Timeout | 1–255 s |
| 5 / 6 | RequestType / ResponseType | default 1 (Text) |
| 7 / 8 | RequestHeaders / RequestBody | |
| 9 / 10 | ResponseHeaders / ResponseBody | after response |
| 11 | StatusCode | HTTP status code; 0 when no response |
| 12 | IsActive | 1 from `SendRequest` until response |

Methods: `SendRequest([body])` (0; argument overrides `RequestBody`), `AbortRequest` (1),
`Clear` (2). Events: `OnRequestSent` (0) right after the request is queued, `OnResponse` (1) after
the response.

- Requests are executed sequentially by a dedicated task (up to 4 queued); the script does not block.
- HTTPS is verified against the built-in ESP-IDF certificate bundle; redirects are followed.
- `SendRequest` while a previous request on the same object is still running aborts/discards the
  previous one (its result is ignored).
- No response (connection error, timeout, response larger than `CLU_HTTP_MAX_BODY`): `OnResponse`
  fires with StatusCode 0, `ResponseBody` nil, and an empty `ResponseHeaders` table; a warning is
  logged. Always check `StatusCode` in the event handler.

## HttpListener (120)

| Index | Property | Notes |
|---|---|---|
| 0 | Path | path this listener handles (e.g. `/light`) |
| 1 | Method | method of the received request |
| 2 | QueryStringParams | parameter table |
| 3 | RequestType | from request `Content-Type` |
| 4 / 5 | RequestHeaders / RequestBody | |
| 6 | ResponseType | default 1 (Text) |
| 7 / 8 | ResponseHeaders / ResponseBody | |
| 9 | StatusCode | default 200 |

Methods: `SendResponse([body])` (0; argument overrides `ResponseBody`), `Clear` (1). Event:
`OnRequest` (0).

- All listeners share a single server on `CLU_HTTP_PORT`. A request is routed to the listener whose
  `Path` equals the request path (exact match, without query string; trailing slash and missing
  leading slash on `Path` are normalized). Missing listener or no `OnRequest` handler: 404.
- Requests are handled sequentially; the response can be sent later (e.g. after calling another CLU
  in the event handler). No `SendResponse` within `CLU_HTTP_LISTENER_TIMEOUT_MS`: 504. An
  `OnRequest` handler that errors before `SendResponse`: 500.
- `clu.await(CluZ->Relay->Switch(0))` from the manual works: remote CLU calls in the event handler
  already wait for the result, and `clu.await` passes it through.
- HTTP/1.1 without keep-alive (`Connection: close`). Unsupported: `Transfer-Encoding: chunked`
  in request (411), body over `CLU_HTTP_MAX_BODY` (413), headers over 4 KB (431).
- **No authentication or encryption**: anyone on the network can call the listeners, just like on a
  real GATE. Check e.g. a custom token header in your script and do not expose the port outside the
  local network.

## Example

```lua
-- OnInit
HTT0001:set(0, "https://api.example.com")
HTT0001:set(1, "/v1/weather")
HTT0001:set(2, { city = "Warszawa" })
HTT0001:set(6, 2)                       -- JSON response
HTT0002:set(0, "/light")

-- HTT0001 OnResponse
if HTT0001:get(11) == 200 then
  temperature = HTT0001:get(10).current.temp
end

-- HTT0002 OnRequest: GET /light?on=1
local q = HTT0002:get(2) or {}
CLU900000001:execute(0, "DOU0001:set(0, " .. (q.on == "1" and 1 or 0) .. ")")
HTT0002:set(6, 2)
HTT0002:execute(0, { ok = true })       -- {"ok":true}
```

Unverified on a real GATE (not specified in the manual): `OnResponse` with StatusCode 0 on
connection error (assumed), whether `Path` also matches by prefix, whether numbers in XML attributes
are strings.
