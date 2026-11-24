# MySensors

Przy `CLU_BUS` = `CLU_MYSENSORS` CLU działa jako bramka sieci [MySensors](https://www.mysensors.org/) na
magistrali RS485: węzły ze szkicami `MY_RS485`, CLU to węzeł 0. Ramki
`SOH to from 'X' len STX <MyMessage> ETX suma EOT` (jak `MyTransportRS485`), prędkość
`MY_RS485_BAUD_RATE` węzłów (tu domyślnie 57600; biblioteka domyślnie 9600). CLU sama odpowiada na to,
na co odpowiada bramka (szukanie rodzica — odległość 0, ping, rejestracja, echo), resztę obsługuje
jak kontroler. Innej bramki na tej magistrali być nie może. Węzły bez identyfikatora
(`MY_NODE_ID` = AUTO) też dołączają: ich rozgłoszenia mają `to` = `from` = 255, które biblioteka po
stronie bramki odrzuca, a CLU przepuszcza (szukanie rodzica, potem `I_ID_REQUEST` → identyfikator).

OM nie widzi wtedy prawdziwych modułów TF-Bus; master TF-Bus działa bez przewodu i obsługuje tylko
moduły emulowane przez firmware (MySensors niżej i wbudowane IO `CLU_LOCAL_IO`).

## Konfiguracja (menuconfig)

`OpenGr8on ESP32 CLU` → `RS485 bus (TF-Bus / MySensors)`: `Protocol` = `MySensors gateway`, tam
też piny (`CLU_RS485_*`, wspólne z TF-Bus) i prędkość, podmenu `MySensors gateway` z opcjami poniżej
i wbudowane IO (`CLU_LOCAL_IO`).

| Opcja | Domyślnie | Opis |
|---|---|---|
| `CLU_RS485_UART` / `TX_GPIO` / `RX_GPIO` | jak TF-Bus | UART i piny |
| `CLU_RS485_DE_GPIO` | jak TF-Bus | pin DE/RE transceivera RS485; `-1` = transceiver z automatycznym kierunkiem |
| `CLU_MYSENSORS_BAUD` | 57600 | `MY_RS485_BAUD_RATE` węzłów (osobna opcja od `CLU_TFBUS_BAUD`, zmiana protokołu jej nie zmienia) |
| `CLU_MYSENSORS_METRIC` | y | odpowiedź na `I_CONFIG`: `M` (metryczne) albo `I` |
| `CLU_MYSENSORS_TRACE` | n | każda wiadomość (w obie strony, jako linia tekstu) w logu na poziomie INFO; w skrypcie `MySensors.trace(true)` |
| `CLU_MYSENSORS_MODULES_MAX` | 4 | najwięcej emulowanych modułów IO_MODULE_DIN_8 (0–8, 0 = brak modułów w OM); tworzone w miarę prezentacji czujników |
| `CLU_LOCAL_IO` | n | wbudowane IO płytki działa także z MySensors, zob. [KinCony F8](devices/kincony-f8.md), [KC868-A8v3](devices/kincony-kc868-a8v3.md) |

## Moduły w Object Manager

Czujniki binarne i wyjścia MySensors widać w OM jako kanały emulowanych modułów
**IO_MODULE_DIN_8** (8 DIN + 8 DOUT). Każdy moduł należy do jednego węzła: czujniki węzła danego
rodzaju zajmują jego moduły w kolejności identyfikatorów dzieci (DOUT 1 = wyjście o najniższym
`child`, …). Węzeł z ponad 8 wyjściami / wejściami dostaje kolejny własny moduł. Czujnik
zaprezentowany później z niższym identyfikatorem przesuwa pozostałe kanały węzła (log `channels
re-sorted`, stany idą za kanałami). Moduły powstają same (nowy numer seryjny, log `new module N …
run discovery in OM`), najwyżej `CLU_MYSENSORS_MODULES_MAX`; moduł opróżniony przez
`MySensors.unmap(node)` dostaje następny węzeł. Numery seryjne, przypisania i właściciele modułów są
w NVS, więc po restarcie czujnik zostaje na tym samym kanale. Po pojawieniu się nowych czujników:
discovery w OM i dodanie nowych modułów do projektu.

Sprzątanie przy discovery w OM: każde discovery najpierw sprawdza węzły — prosi każdy znany węzeł o
ponowną prezentację (`I_PRESENTATION`), po kolei: następny, gdy tylko poprzedni odpowie (pierwsza
ramka po ~50 ms), a na milczący czeka 0,5 s. Potem usuwa kanały węzłów, które nie odpowiedziały
(węzeł zniknął, wrócił pod nowym identyfikatorem, …), i moduły bez kanałów; pozostałe moduły
zachowują numery seryjne. Identyfikatory takich węzłów są zwalniane (NVS): `MySensors.nodes()` ich nie
zwraca, a `I_ID_REQUEST` może je przydzielić nowemu węzłowi. W logu: `discovery: N of M node(s)
answered the node check`. Gdy sprawdzanie nie skończy się w czasie, discovery niczego nie usuwa.
Węzeł uśpiony, który nie odpowie, też traci kanały — wracają przy jego następnej prezentacji (nowy
moduł, nowe discovery). Takie samo sprawdzanie (bez usuwania) uruchamia się 2 s po starcie: prezentacje
węzłów wypełniają rejestr.

| Typ MySensors | Kanał | Wartość |
|---|---|---|
| `S_DOOR`, `S_MOTION`, `S_SMOKE`, `S_WATER_LEAK`, `S_SOUND`, `S_VIBRATION`, `S_MOISTURE` | DIN | `V_TRIPPED` |
| `S_BINARY` / `S_LIGHT`, `S_SPRINKLER` | DOUT | `V_STATUS` |
| `S_LOCK` | DOUT | `V_LOCK_STATUS` |

- DOUT z OM → `C_SET` do węzła; stan zgłoszony przez węzeł (albo wysłany skryptem) wraca na DOUT.
- Bramka offline (`MySensors.status().connected` = false) → moduły nie odpowiadają, OM widzi je
  jako niedostępne.
- `MySensors.slots()` — który czujnik na którym kanale; `MySensors.unmap(node [, child])` zwalnia
  kanały usuniętego węzła.
- Wartości liczbowe (temperatura, wilgotność, …) zostają w skryptach: Grenton nie ma modułu TF-Bus,
  który by je przeniósł. Planowane: ściemniacz (`S_DIMMER`), rolety (`S_COVER`), LED RGBW
  (`S_RGB_LIGHT`) jako kolejne emulowane typy modułów.

## Sprawdzone urządzenia

| Urządzenie | W OM |
|---|---|
| nippy™ SIMPLE DIN Input/Relay 6/6 | jeden IO_MODULE_DIN_8: wejścia DIN 1–6, przekaźniki DOUT 1–6 |

## Diagnostyka

Co 30 s, jeśli coś przyszło, firmware ocenia magistralę i loguje zmianę stanu
(`MySensors.status().link`):

| Stan | Log | Znaczy |
|---|---|---|
| `ok` | `RS485 network: valid frames from the nodes` | węzły nadają poprawne ramki |
| `garbage` | `RS485 network: N byte(s), M bad frame(s), no valid one…` + `first bytes: <hex> \|<ascii>\|` | bajty są, poprawnych ramek brak: zła prędkość (`CLU_MYSENSORS_BAUD` ≠ `MY_RS485_BAUD_RATE`; bity dłuższe niż nasze = wolniejsi nadawcy), zamienione A/B, brak polaryzacji |

Cisza nie jest błędem (węzły mówią, gdy coś się dzieje). Przy dobrej prędkości ramka zaczyna się od
`01 … 58 … 02` (SOH, adresy, `'X'`, długość, STX). `MySensors.trace(true)` (albo
`CLU_MYSENSORS_TRACE`) pokazuje każdą wiadomość jako linię tekstu: `<` od węzła, `(broadcast)`,
`>` do węzła.

## Kontroler i skrypty

Kontroler (firmware, bez skryptów):

- `I_ID_REQUEST` — najniższy wolny identyfikator węzła 1–254, zapisany w NVS (węzły ze stałym
  identyfikatorem też go rezerwują);
- `I_CONFIG` — `M` / `I` (`CLU_MYSENSORS_METRIC`), `I_TIME` — czas lokalny w sekundach od 1970
  (po ustawieniu zegara);
- `C_REQ` — odpowiedź ostatnią znaną wartością (z węzła albo wysłaną przez skrypt);
- prezentacje, nazwa / wersja szkicu, poziom baterii — rejestr węzłów (`MySensors.nodes()`);
- własne ramki wracające przez odbiornik RS485 są pomijane (nadawca 0), wysyłka dopiero po kilku ms
  ciszy na magistrali, po jednej ramce co 25 ms (węzeł ma bufor na jedną wiadomość i nie
  potwierdza odbioru: seria ramek, np. kilka przekaźników naraz, gubiła dalsze komendy).

Nie ma aktualizacji firmware węzłów (`C_STREAM`) ani kolejkowania wiadomości dla uśpionych węzłów.

W USER.LUA:

```lua
local V = MySensors.V
MySensors.on(12, 1, V.V_TEMP, function(t) VAR_TEMP:set(0, t) end)   -- C_SET from node 12, child 1
MySensors.on(nil, nil, V.V_TRIPPED, function(v, msg) print(msg.node, v) end) -- nil = any
MySensors.onMessage(function(msg) end)          -- every message: node, child, cmd, ack, type, payload
MySensors.send(13, 7, V.V_STATUS, true)         -- C_SET (true/false -> 1/0), optional ack
MySensors.request(13, 7, V.V_STATUS)            -- C_REQ
MySensors.internal(13, 255, MySensors.I.I_REBOOT)
local value, age_ms = MySensors.get(12, 1, V.V_TEMP)
MySensors.nodes()                               -- {[id] = {sketch, version, lib, battery, age_ms, children}}
MySensors.status()                              -- {connected, version, rx, tx, dropped, bad, rx_bytes, link, trace}
MySensors.trace(true)                           -- every gateway line in the log (INFO)
MySensors.slots()                               -- sensors on channels of the emulated modules
MySensors.unmap(12)                             -- free node 12's channels
```

Wartości są liczbami, a dla typów tekstowych (`V_TEXT`, `V_RGB`, `V_ID`, …) tekstem. Pełny opis:
[mysensors.lua](../components/clu_mysensors/mysensors.lua).

## Kod i testy

| Plik | Rola |
|---|---|
| `components/clu_proto/ms_proto.c` | wiadomość jako linia tekstu (log, testy) |
| `components/clu_proto/ms_rs485.c` | ramki sieci RS485 (`MY_RS485`) i binarna MyMessage ↔ wiadomość kontrolera |
| `components/clu_proto/tfbus_vmod.c` | strona modułu TF-Bus dla modułów emulowanych przez firmware (także wbudowane IO) |
| `components/clu_mysensors/clu_mysensors.c` | bramka sieci RS485 i kontroler: UART, identyfikatory węzłów (NVS), pamięć wartości, diagnostyka magistrali, API Lua |
| `components/clu_mysensors/ms_vmod.c` | czujniki jako kanały emulowanych IO_MODULE_DIN_8 (przypisania i numery seryjne w NVS) |
| `components/clu_mysensors/mysensors.lua` | `MySensors.*` dla USER.LUA |

Testy hosta (`make -C test/host test`): `test_mysensors` (ramki RS485 i wszystkie typy danych
MyMessage; CLU jako bramka: szukanie rodzica, ping, rejestracja, echo, identyfikatory, wartości,
ramki do węzłów, cisza magistrali przed wysłaniem, stany magistrali; kontroler na symulowanym UART i
NVS: identyfikatory, `I_CONFIG` / `I_TIME`, `C_REQ`, API Lua; emulowane moduły ramkami TF-Bus: discovery, adresowanie, status, wyjścia do
węzłów, przypisania), `test_mysensors.lua` (API `MySensors.*`), `test_main_mysensors` (start z
MySensors: master TF-Bus bez przewodu), `test_uart` (kilka punktów końcowych, tryb bez przewodu).

---

# MySensors

With `CLU_BUS` = `CLU_MYSENSORS`, the CLU acts as a [MySensors](https://www.mysensors.org/) network
gateway on the RS485 bus: nodes run `MY_RS485` sketches, and the CLU is node 0. Frames use
`SOH to from 'X' len STX <MyMessage> ETX checksum EOT` (same as `MyTransportRS485`), at the nodes'
`MY_RS485_BAUD_RATE` (default 57600 here; library default is 9600). The CLU itself answers gateway
messages (parent search — distance 0, ping, registration, echo) and handles the rest as a
controller. There must be no other gateway on the same bus. Nodes without an ID (`MY_NODE_ID` = AUTO)
also join: their broadcasts have `to` = `from` = 255, which the library drops on the gateway side,
while the CLU lets them through (parent search, then `I_ID_REQUEST` → node ID).

In this mode OM does not see real TF-Bus modules; the TF-Bus master runs wire-free and only handles
firmware-emulated modules (MySensors below and built-in IO `CLU_LOCAL_IO`).

## Configuration (menuconfig)

`OpenGr8on ESP32 CLU` → `RS485 bus (TF-Bus / MySensors)`: `Protocol` = `MySensors gateway`, plus
pins (`CLU_RS485_*`, shared with TF-Bus) and baud rate, `MySensors gateway` submenu with the options
below, and built-in IO (`CLU_LOCAL_IO`).

| Option | Default | Description |
|---|---|---|
| `CLU_RS485_UART` / `TX_GPIO` / `RX_GPIO` | same as TF-Bus | UART and pins |
| `CLU_RS485_DE_GPIO` | same as TF-Bus | RS485 transceiver DE/RE pin; `-1` = auto-direction transceiver |
| `CLU_MYSENSORS_BAUD` | 57600 | nodes' `MY_RS485_BAUD_RATE` (separate from `CLU_TFBUS_BAUD`, switching protocols leaves it unchanged) |
| `CLU_MYSENSORS_METRIC` | y | reply to `I_CONFIG`: `M` (metric) or `I` |
| `CLU_MYSENSORS_TRACE` | n | log every message (both directions, as a text line) at INFO level; in script `MySensors.trace(true)` |
| `CLU_MYSENSORS_MODULES_MAX` | 4 | max emulated IO_MODULE_DIN_8 modules (0–8, 0 = no modules in OM); created as sensors present themselves |
| `CLU_LOCAL_IO` | n | board's built-in IO also works with MySensors, see [KinCony F8](devices/kincony-f8.md), [KC868-A8v3](devices/kincony-kc868-a8v3.md) |

## Modules in Object Manager

MySensors binary sensors and outputs appear in OM as channels of emulated **IO_MODULE_DIN_8**
modules (8 DIN + 8 DOUT). Each module belongs to a single node: a node's sensors of a given kind
occupy its modules in child ID order (DOUT 1 = output with the lowest `child`, …). A node with more
than 8 outputs / inputs gets another module of its own. A sensor presented later with a lower ID
shifts the node's remaining channels (log `channels re-sorted`, states follow the channels). Modules
are created automatically (new serial number, log `new module N … run discovery in OM`), up to
`CLU_MYSENSORS_MODULES_MAX`; a module freed by `MySensors.unmap(node)` is reused by the next node.
Serial numbers, mappings, and module owners are stored in NVS, so after a reboot each sensor stays
on the same channel. When new sensors appear: run discovery in OM and add the new modules to the
project.

Cleanup during OM discovery: each discovery first checks nodes — it asks every known node to
re-present (`I_PRESENTATION`), one by one: moving to the next as soon as the previous replies (first
frame after ~50 ms), or waiting 0.5 s for a silent node. It then removes channels of nodes that did
not reply (node gone, returned under a new ID, …) and empty modules; remaining modules keep their
serial numbers. Such nodes' IDs are released (NVS): `MySensors.nodes()` no longer returns them, and
`I_ID_REQUEST` can assign them to a new node. In the log: `discovery: N of M node(s) answered the
node check`. If the check does not finish in time, discovery removes nothing. A sleeping node that
does not reply also loses its channels — they come back on its next presentation (new module, new
discovery). The same check (without removal) runs 2 s after boot so node presentations populate the
registry.

| MySensors type | Channel | Value |
|---|---|---|
| `S_DOOR`, `S_MOTION`, `S_SMOKE`, `S_WATER_LEAK`, `S_SOUND`, `S_VIBRATION`, `S_MOISTURE` | DIN | `V_TRIPPED` |
| `S_BINARY` / `S_LIGHT`, `S_SPRINKLER` | DOUT | `V_STATUS` |
| `S_LOCK` | DOUT | `V_LOCK_STATUS` |

- DOUT from OM → `C_SET` to the node; state reported by the node (or sent from a script) updates
  DOUT.
- Gateway offline (`MySensors.status().connected` = false) → modules do not respond, OM sees them as
  unavailable.
- `MySensors.slots()` — which sensor is on which channel; `MySensors.unmap(node [, child])` frees
  channels of a removed node.
- Numeric values (temperature, humidity, …) stay in scripts: Grenton has no TF-Bus module that
  carries them. Planned: dimmer (`S_DIMMER`), roller shutter (`S_COVER`), RGBW LED (`S_RGB_LIGHT`) as
  additional emulated module types.

## Tested devices

| Device | In OM |
|---|---|
| nippy™ SIMPLE DIN Input/Relay 6/6 | one IO_MODULE_DIN_8: inputs DIN 1–6, relays DOUT 1–6 |

## Diagnostics

Every 30 s, if any bytes arrived, the firmware evaluates the bus and logs state changes
(`MySensors.status().link`):

| State | Log | Meaning |
|---|---|---|
| `ok` | `RS485 network: valid frames from the nodes` | nodes are sending valid frames |
| `garbage` | `RS485 network: N byte(s), M bad frame(s), no valid one…` + `first bytes: <hex> \|<ascii>\|` | bytes arrive, no valid frames: wrong baud (`CLU_MYSENSORS_BAUD` ≠ `MY_RS485_BAUD_RATE`; bits longer than ours = slower senders), swapped A/B, missing biasing |

Silence is not an error (nodes speak when something happens). At the right baud rate, a frame starts
with `01 … 58 … 02` (SOH, addresses, `'X'`, length, STX). `MySensors.trace(true)` (or
`CLU_MYSENSORS_TRACE`) prints every message as a text line: `<` from node, `(broadcast)`, `>` to
node.

## Controller and scripts

Controller (firmware, no scripts needed):

- `I_ID_REQUEST` — lowest free node ID 1–254, saved in NVS (nodes with a fixed ID also reserve it);
- `I_CONFIG` — `M` / `I` (`CLU_MYSENSORS_METRIC`), `I_TIME` — local time in seconds since 1970 (once
  clock is set);
- `C_REQ` — replies with the last known value (from the node or sent by a script);
- presentations, sketch name / version, battery level — node registry (`MySensors.nodes()`);
- own frames echoed back by the RS485 receiver are ignored (sender 0), transmission only starts
  after a few ms of bus silence, one frame every 25 ms (nodes have a single-message buffer and do not
  ack receipt: a burst of frames, e.g. multiple relays at once, dropped subsequent commands).

Node firmware updates (`C_STREAM`) and message queuing for sleeping nodes are not supported.

In USER.LUA:

```lua
local V = MySensors.V
MySensors.on(12, 1, V.V_TEMP, function(t) VAR_TEMP:set(0, t) end)   -- C_SET from node 12, child 1
MySensors.on(nil, nil, V.V_TRIPPED, function(v, msg) print(msg.node, v) end) -- nil = any
MySensors.onMessage(function(msg) end)          -- every message: node, child, cmd, ack, type, payload
MySensors.send(13, 7, V.V_STATUS, true)         -- C_SET (true/false -> 1/0), optional ack
MySensors.request(13, 7, V.V_STATUS)            -- C_REQ
MySensors.internal(13, 255, MySensors.I.I_REBOOT)
local value, age_ms = MySensors.get(12, 1, V.V_TEMP)
MySensors.nodes()                               -- {[id] = {sketch, version, lib, battery, age_ms, children}}
MySensors.status()                              -- {connected, version, rx, tx, dropped, bad, rx_bytes, link, trace}
MySensors.trace(true)                           -- every gateway line in the log (INFO)
MySensors.slots()                               -- sensors on channels of the emulated modules
MySensors.unmap(12)                             -- free node 12's channels
```

Values are numbers, or strings for text types (`V_TEXT`, `V_RGB`, `V_ID`, …). Full reference:
[mysensors.lua](../components/clu_mysensors/mysensors.lua).

## Code and tests

| File | Role |
|---|---|
| `components/clu_proto/ms_proto.c` | message as a text line (log, tests) |
| `components/clu_proto/ms_rs485.c` | RS485 network frames (`MY_RS485`) and binary MyMessage ↔ controller message |
| `components/clu_proto/tfbus_vmod.c` | TF-Bus module side for firmware-emulated modules (including built-in IO) |
| `components/clu_mysensors/clu_mysensors.c` | RS485 network gateway and controller: UART, node IDs (NVS), value cache, bus diagnostics, Lua API |
| `components/clu_mysensors/ms_vmod.c` | sensors as channels of emulated IO_MODULE_DIN_8 (mappings and serial numbers in NVS) |
| `components/clu_mysensors/mysensors.lua` | `MySensors.*` for USER.LUA |

Host tests (`make -C test/host test`): `test_mysensors` (RS485 frames and all MyMessage data types;
CLU as gateway: parent search, ping, registration, echo, IDs, values, frames to nodes, bus silence
before sending, bus states; controller on simulated UART and NVS: IDs, `I_CONFIG` / `I_TIME`,
`C_REQ`, Lua API; emulated modules via TF-Bus frames: discovery, addressing, status, outputs to
nodes, mappings), `test_mysensors.lua` (`MySensors.*` API), `test_main_mysensors` (boot with
MySensors: wire-free TF-Bus master), `test_uart` (multiple endpoints, wire-free mode).
