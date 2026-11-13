# MySensors

Z `CLU_BUS` = `CLU_MYSENSORS` CLU jest bramką sieci [MySensors](https://www.mysensors.org/) na
magistrali RS485: węzły ze szkicami `MY_RS485`, CLU to węzeł 0. Ramki
`SOH to from 'X' len STX <MyMessage> ETX suma EOT` (jak `MyTransportRS485`), prędkość
`MY_RS485_BAUD_RATE` węzłów (tu domyślnie 57600; biblioteka domyślnie 9600). CLU sam odpowiada na to,
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
| `CLU_MYSENSORS_BAUD` | 57600 | `MY_RS485_BAUD_RATE` węzłów (osobna opcja od `CLU_TFBUS_BAUD`, zmiana protokołu jej nie rusza) |
| `CLU_MYSENSORS_METRIC` | y | odpowiedź na `I_CONFIG`: `M` (metryczne) albo `I` |
| `CLU_MYSENSORS_TRACE` | n | każda wiadomość (w obie strony, jako linia tekstu) w logu na poziomie INFO; w skrypcie `MySensors.trace(true)` |
| `CLU_MYSENSORS_MODULES_MAX` | 4 | najwięcej emulowanych modułów IO_MODULE_DIN_8 (0–8, 0 = brak modułów w OM); tworzone w miarę prezentacji czujników |
| `CLU_LOCAL_IO` | n | wbudowane IO płytki działa także z MySensors, zob. [DEVICES.md](DEVICES.md#kincony-f8-i-kc868-a8v3) |

## Moduły w Object Manager

Czujniki binarne i wyjścia MySensors widać w OM jako kanały emulowanych modułów
**IO_MODULE_DIN_8** (8 DIN + 8 DOUT). Każdy moduł należy do jednego węzła: czujniki węzła danego
rodzaju zajmują jego moduły w kolejności identyfikatorów dzieci (DOUT 1 = wyjście o najniższym
`child`, …). Węzeł z więcej niż 8 wyjściami / wejściami dostaje kolejny własny moduł. Czujnik
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
moduł, nowe discovery). Takie samo sprawdzanie (bez usuwania) biegnie 2 s po starcie: prezentacje
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
  ciszy na magistrali.

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
[mysensors.lua](components/clu_mysensors/mysensors.lua).

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
