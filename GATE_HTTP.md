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
| 18 / 19 | UseCloud / CloudConnection | połączenie MQTT, jak na CLU ([MQTT](README.md#mqtt--home-assistant)) |
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
