# opengr8on-clu-esp32 — sprzętowa CLU Grentona na ESP32-S3

[![Tests](https://github.com/psobiech/opengr8on-clu-esp32/actions/workflows/tests.yml/badge.svg?branch=develop)](https://github.com/psobiech/opengr8on-clu-esp32/actions/workflows/tests.yml)
[![Firmware](https://github.com/psobiech/opengr8on-clu-esp32/actions/workflows/firmware.yml/badge.svg?branch=develop)](https://github.com/psobiech/opengr8on-clu-esp32/actions/workflows/firmware.yml)

> **Uwaga / Note:** Dokumentacja, opisy protokołów i zachowanie firmware w tym repozytorium mogą
> zawierać błędy, nieścisłości lub domysły. Nie gwarantujemy ich dokładności ani kompletności.
> Oprogramowanie jest dostarczane bez żadnej gwarancji — zob. [Zastrzeżenie prawne](#zastrzeżenie-prawne).
>
> Documentation, protocol descriptions and firmware behaviour in this repository may contain errors,
> inaccuracies, or guesswork. No guarantee of accuracy or completeness is provided. The software
> comes with no warranty — see [Disclaimer](#disclaimer).

Firmware, który z ESP32-S3 (R8: 16 MB flash, 8 MB PSRAM) i transceivera RS485 robi jednostkę
centralną (CLU) zgodną z systemem Grenton. Object Manager (OM) — albo rozszerzenie VS Code
[opengr8on-vscode](https://github.com/psobiech/opengr8on-vscode) w jego roli — widzi ją jak zwykłą
CLU: wykrywa ją, wgrywa projekt (OM.LUA / USER.LUA), steruje obiektami i odczytuje diagnostykę.
Po stronie RS485 CLU jest masterem magistrali TF-Bus i obsługuje prawdziwe moduły Grentona.

To port w C vCLU z [opengr8on-node](https://github.com/psobiech/opengr8on-node): strona UDP/Lua
pochodzi z vCLU, strona RS485 z jego `src/tfbus`. Opis protokołów: `docs/UDP.md` i `docs/TFBUS.md`.
Wszystkie ścieżki `docs/...` w tym pliku odnoszą się do repozytorium opengr8on-node.

> **Wczesna faza rozwoju.** Większość funkcji jest przetestowana na hoście (testy jednostkowe,
> porównanie z przechwytem prawdziwej CLU), ale część zachowań jest odgadnięta i niezweryfikowana na
> sprzęcie.
>
> **Pokrycie testami** (`make -C test/host coverage`, mierzone też w CI): ok. 98% linii całego kodu C
> firmware (każdy plik `components/` i `main/` ma testy hosta; bez zewnętrznej biblioteki Lua) i ok.
> 97% kodu Lua (prelude, obiekty wirtualne, MQTT). Testy hosta zastępują ESP-IDF zaślepkami: nie
> sprawdzają WiFi, FAT, UART ani zegarów na prawdziwym sprzęcie.

## Spis treści

- [Architektura](#architektura)
- [Zgodność z CLU_ZWAVE_2](#zgodność-z-clu_zwave_2)
- [Funkcje ponad CLU_ZWAVE_2](#funkcje-ponad-clu_zwave_2)
- [Sprzęt](#sprzęt)
- [Budowanie i wgrywanie](#budowanie-i-wgrywanie)
- [Konfiguracja](#konfiguracja)
- [MQTT / Home Assistant](#mqtt--home-assistant)
- [Testy](#testy)
- [Bezpieczeństwo](#bezpieczeństwo)
- [Licencje](#licencje)
- [Zastrzeżenie prawne](#zastrzeżenie-prawne)

## Architektura

| Komponent | Rola |
|---|---|
| `main/` | start, WiFi/Ethernet/IP (`clu_net`, `clu_eth`), czas/SNTP/RTC (`clu_time`), serwer UDP/1234 (`udp_server`), diagnostyka OM (`clu_diag`), stan CLU (`clu_state`) |
| `components/clu_crypto` | AES-128-CBC/PKCS#7 (PSA crypto, mbedtls 4) |
| `components/clu_proto` | wyprowadzanie kluczy, ramkowanie TF-Bus, protokół TFTP (czysty C, testowany na hoście) |
| `components/clu_lua` | środowisko Lua: prelude (model obiektów OM), obiekty wirtualne (`objects.lua`) |
| `components/lua` | Lua 5.3.6 (domyślnie, jak w CLU) i 5.5.1 (opcja `CLU_LUA_5_5`), źródła niezmodyfikowane |
| `components/tfbus_uart` | UART RS485 half-duplex |
| `components/tfbus_master` | master magistrali: rejestr modułów, adresowanie, odpytywanie, discovery |
| `components/tfbus_drivers` | sterowniki modułów TF-Bus |
| `components/clu_tftp` | serwer TFTP (a:\ i p:\) |
| `components/clu_telnet` | konsola logów (TCP 23, tylko odczyt) |
| `components/clu_mqtt` | MQTT + discovery Home Assistant |
| `components/clu_ota` | aktualizacja firmware przez HTTP z hasłem (`POST /ota`), potwierdzanie nowego obrazu |
| `components/clu_rtc` | zegar RTC PCF85063 albo DS3231 na I2C (podtrzymywany bateryjnie) |
| `components/clu_i2c` | wspólne magistrale I2C (jedna na parę pinów: RTC + ekspander IO) |
| `components/clu_localio` | IO płytki (PCF8575) jako moduł IO_MODULE_DIN_8 odpowiadający na ramki TF-Bus |
| `tools/mknvs.py` | obraz partycji `cfg` z ustawieniami dla gotowych obrazów (seryjny, PIN, WiFi, MQTT) |
| `web/` | flasher w przeglądarce (GitHub Pages): obraz z Releases + ustawienia generowane w stronie (`web/nvs.js`) |

Partycje (`partitions.csv`): NVS (klucze, PIN, sieć, czas), `cfg` (12 KB pod `0x12000`, ustawienia
z `mknvs.py`, firmware tylko je czyta), dwie partycje aplikacji po 4 MB, FAT 6 MB jako `a:\`
(projekt, CONFIG.JSON, STORAGE.DAT, certyfikaty).

## Zgodność z CLU_ZWAVE_2

Punkt odniesienia: CLU_ZWAVE_2 z firmware 5.15.24 (build 2635; raportowane fwApiVer 0x203) i Object Manager 1.13.x. Legenda: ✅ gotowe,
🟡 gotowe, ale niezweryfikowane na prawdziwym sprzęcie / semantyka odgadnięta, ❌ brak.

### Protokół UDP i usługi CLU

| Obszar | Stan | Uwagi |
|---|---|---|
| Discovery, `req_set_key`, `req_reset`, `req:` (Lua), clientReport | ✅ | AES-128-CBC, klucze jak w `docs/UDP.md` |
| `req_set_clu_ip` | 🟡 | statyczny IP w NVS, zakładana maska /24, `0.0.0.0` = powrót do DHCP; DNS z cech 25/26 |
| Czas: SNTP (cecha 22), TimeZone (23), OnTimeChange (zdarzenie 13) | ✅ | zapisywane w NVS |
| TFTP (`req_start_ftp` / `req_tftp_stop`, octet + netascii) | ✅ | |
| Diagnostyka OM (`req_diagnostic_refresh` → `m:\DIAGNOSTIC_PACK.JSON`) | ✅ | napięcie zawsze 0 (brak pomiaru) |
| Logi w OM (Diagnostyka › Logi, telnet TCP 23) | ✅ | |
| `req_refresh_modules` (wyszukiwanie nowych modułów) | 🟡 | odpowiedź DISCOVER prawdziwych modułów nieprzechwycona |

### Lua i obiekty

| Obszar | Stan | Uwagi |
|---|---|---|
| Lua 5.3.6, MAIN/OM/USER.LUA, tryb awaryjny, OBJECT/SYSTEM | ✅ | prelude przetestowane z prawdziwym OM.LUA |
| Lokalna CLU (klasa 0): Uptime, State, Date/Time, AddToLog, SetDateTime | ✅ | |
| Zdalna CLU (klasa 1): `execute(0, "lua")`, `get`/`set`/`execute` | ✅ | w zdarzeniach wywołanie czeka na odpowiedź (korutyna) |
| Timer, Calendar (cron), EventScheduler, PresenceSensor v2, Storage | 🟡 | Storage: trwałe zmienne w `a:\STORAGE.DAT` |
| Scheduler, Thermostat v2, SunriseSunsetCalendar v3 | 🟡 | format danych zgodny z OM, sterowanie niezweryfikowane |
| Push | 🟡 | tylko wpis do logu (brak chmury Grentona) |
| MultiFanACThermostat, PIDcontroller | ❌ | |

### Moduły TF-Bus

| Moduł | Stan | Uwagi |
|---|---|---|
| Master: RESET/SEEK/ADDR_ASSIGN, odpytywanie, obecność | ✅ | kolejność zweryfikowana z przechwytem |
| Nowe moduły: DISCOVER (0x08), VERSION_QUERY, `tfbusDevices` w `a:\CONFIG.JSON` | 🟡 | moduły offline adresowane ponownie |
| IO_MODULE_DIN_8 (0x1e), RELAY_DIN_4 (0x15) | ✅ | DOUT/DIN, czasowe Switch*, click/hold ze zboczy wejść |
| ROLLER_SH_DIN_3 (0x2a), ROLLER_SH_DIN (0x17) | 🟡 | maska bitowa ruch/stop niezweryfikowana |
| TOUCH_PANEL_FM_4 (0x1c) | 🟡 | przyciski, temperatura, światło, buzzer; LED niezweryfikowane |
| SMART_PANEL_FM_4 (0x03) | 🟡 | przyciski, czujniki, LED, PrintText/Draw/Beep; brak konfiguracji stron/gestów |
| DIMMER_MOSFET_DIN (0x1a) | 🟡 | DIMM: SetValue/Switch*/HoldValue, rampa, Min/Max, LastValue; ramki zgodne z przechwytem, semantyka zdarzeń/Hold niezweryfikowana |
| LED_RGBW_DIN (0x18) | 🟡 | LEDRGB: model HSV/RGB + W w CLU, korekcja bieli, Switch*/Hold*; PowerSupplyVoltage; ramki zgodne z przechwytem, LED_CHANNEL/LED_CCT brak |
| Pozostałe moduły TF-Bus (ściemniacze FM, LED FM, analogowe itd.) | ❌ | brak sterowników |
| Wbudowane IO płytki (PCF8575) jako IO_MODULE_DIN_8 | 🟡 | KinCony F8, KC868-A8v3, zob. [DEVICES.md](DEVICES.md#kincony-f8-i-kc868-a8v3); niesprawdzone na sprzęcie |

### Czego brakuje względem CLU_ZWAVE_2

- **Z-Wave** — brak radia; węzły Z-Wave z OM.LUA są pomijane.
- **Ethernet** — wewnętrzny EMAC tylko na targecie `esp32` (LAN8720A na sprzęcie Grenton GATE, inne
  PHY RMII przez sterownik generyczny, np. JL1101); ESP32-S3 nie ma EMAC, więc tam Ethernet tylko
  przez W5500 na SPI (KinCony F8, KC868-A8v3) albo wyłącznie WiFi.
- **Pomiar napięcia magistrali** (cechy 27–30, zdarzenia OnBusVoltage*) — brak ADC na płytce.
- **Chmura Grentona** (aplikacja mobilna, Push) — świadomie wyłączona; zamiast niej MQTT + Home Assistant.
- **Aktualizacja firmware z OM** — firmware wgrywa się przez `idf.py flash`.
- Sterowniki większości typów modułów, konfiguracja stron SMART_PANEL, MultiFanACThermostat,
  PIDcontroller.
- Cechy QoS (24) i TelnetLogLevel (31) są tylko zapamiętywane.

## Funkcje ponad CLU_ZWAVE_2

- **MQTT + discovery Home Assistant** — encje HA tworzone automatycznie z OM.LUA, z nazwami z OM;
  sterowanie przekaźnikami, roletami, zdarzenia przycisków, czujniki paneli. Szczegóły:
  [MQTT / Home Assistant](#mqtt--home-assistant).
- **Diagnostyka firmware w HA** — heap, PSRAM, pamięć Lua, RSSI, IP, przyczyna resetu, liczniki
  TF-Bus, obecność każdego modułu, przycisk Restart.
- **Surowe API obiektów przez MQTT** (opcjonalne) — get/set dowolnej cechy, wywołanie dowolnej
  metody.
- **`MQTT.*` w USER.LUA** — własne mapowania encji, ukrywanie, publikowanie dowolnych tematów.
- **Bez chmury Grentona** — OM nie rejestruje CLU w chmurze (zob. niżej).
- **Aktualizacja firmware przez sieć** (OTA, HTTP z hasłem, cofanie nieudanej aktualizacji).
- **Lua 5.5** jako opcja kompilacji.
- **Wzmocnienia bezpieczeństwa** zgodne z OM; jedyna zmiana obsługi: discovery, klucz i TFTP tylko w oknie
  konfiguracji po starcie lub po wciśnięciu BOOT (zob. [Bezpieczeństwo](#bezpieczeństwo)).
- **Testy hosta** porównujące sterowniki bajt po bajcie z przechwytem prawdziwej CLU.

## Sprzęt

Piny, profile i uwagi do każdej płytki: [DEVICES.md](DEVICES.md).

| Płytka | Układ | Sieć | Uwagi |
|---|---|---|---|
| [Waveshare ESP32-S3-RS485-CAN](DEVICES.md#waveshare-esp32-s3-rs485-can) | ESP32-S3R8 | WiFi | zalecana; RS485 i RTC na płytce |
| [Inne płytki ESP32-S3](DEVICES.md#inne-płytki-esp32-s3) | ESP32-S3, 16 MB / 8 MB PSRAM | WiFi | własny transceiver RS485 |
| [KinCony F8, KC868-A8v3](DEVICES.md#kincony-f8-i-kc868-a8v3) | ESP32-S3 N16R8 | Ethernet W5500 | 8 przekaźników + 8 wejść jako IO_MODULE_DIN_8 |
| [Grenton GATE](DEVICES.md#grenton-gate) | ESP32-WROVER-B | Ethernet LAN8720A | target `esp32` |

## Budowanie i wgrywanie

### Gotowe obrazy

Bez instalowania ESP-IDF. Każde wydanie na stronie
[Releases](https://github.com/psobiech/opengr8on-clu-esp32/releases) ma dla każdej płytki pełny
obraz `opengr8on-clu-<płytka>-<wersja>.bin` (instalacja / aktualizacja przez USB) i sam obraz
aplikacji `opengr8on-clu-<płytka>-<wersja>-ota.bin` ([aktualizacja przez sieć](#aktualizacja-przez-sieć-ota)):

| Płytka | `<płytka>` | Układ | Sieć |
|---|---|---|---|
| Waveshare ESP32-S3-RS485-CAN | `waveshare` | `esp32s3` | WiFi |
| Grenton GATE | `gate` | `esp32` | Ethernet (DHCP) |
| KinCony F8 | `f8` | `esp32s3` | Ethernet (DHCP) |
| KinCony KC868-A8v3 | `a8v3` | `esp32s3` | Ethernet (DHCP) |

Wgrywa się go
**[flasherem w przeglądarce](https://psobiech.github.io/opengr8on-clu-esp32/)** (Chrome / Edge na
komputerze, Web Serial): wybierz w nim obraz `.bin` płytki i tryb: **Instalacja**
(kasuje cały flash), **Aktualizacja** (stan i dysk `a:\` zostają) albo **Tylko ustawienia**.
Ustawienia (seryjny, PIN, WiFi, MQTT) są zapisywane tylko po zaznaczeniu **Zapisz nowe
ustawienia**; bez tego zostają te z urządzenia (instalacja odczytuje je przed kasowaniem i wgrywa
z powrotem). Obraz ustawień powstaje w przeglądarce (ten sam co z `mknvs.py`,
hasła nigdzie nie są wysyłane); można go też pobrać jako `cfg.bin`. Flasher sprawdza, czy obraz
pasuje do podłączonego układu, a po wgraniu pokazuje log UART.

### Aktualizacja przez sieć (OTA)

Z hasłem OTA (`--ota-pass` w `mknvs.py`, pole w flasherze w przeglądarce albo `CLU_OTA_PASSWORD`)
CLU przyjmuje nowy firmware przez HTTP — obraz aplikacji `opengr8on-clu-<płytka>-<wersja>-ota.bin`
z wydania (w paczce / artefakcie `opengr8on-clu-<płytka>-ota.bin`, to samo co
`opengr8on_clu_esp32.bin` z budowania; nie pełny obraz). Bez hasła serwer nie startuje.

**Z przeglądarki:** otwórz `http://IP_CLU:8080/`, wybierz plik, wpisz hasło, **Update**. Strona
najpierw sprawdza hasło, potem wysyła obraz z paskiem postępu; po restarcie (~20 s) pokazuje nową
wersję. Działa w każdej przeglądarce (strona i `/ota` to ten sam adres).

**Z linii poleceń:**

```bash
curl -u ota:HASLO --data-binary @opengr8on-clu-gate-v0.0.3-ota.bin http://IP_CLU:8080/ota
```

Nazwa użytkownika jest dowolna. Obraz trafia do drugiego slotu OTA, jest weryfikowany (układ,
SHA-256, nazwa projektu), a CLU restartuje się do niego; ustawienia, stan i dysk `a:\` zostają.
Bootloader cofa do poprzedniego firmware, jeśli nowy zrestartuje się, zanim dostanie IP
(`BOOTLOADER_APP_ROLLBACK_ENABLE`; wymaga bootloadera z tej wersji lub nowszej, wgranego raz przez
USB/UART). Odpowiedzi: `200` — wgrane, restart; `401` — złe hasło (każda próba ~2 s); `400` — nie ten
plik / inny układ; `411` — brak obrazu; `413` — obraz większy niż slot.

**Z [flashera w przeglądarce](https://psobiech.github.io/opengr8on-clu-esp32/):** tryb **Przez sieć
(OTA)** w kroku 3 (domyślnie nadal wgrywanie przez USB) — adres CLU i hasło OTA; przyjmuje pełny
obraz z wydania (wycina z niego aplikację) albo `…-ota.bin`. CLU wysyła nagłówki
CORS dla `CLU_OTA_CORS_ORIGIN` (domyślnie `https://psobiech.github.io`), ale strona HTTPS dosięgnie
adresu HTTP w sieci lokalnej tylko w przeglądarce z dostępem do sieci lokalnej (Chrome, po zgodzie
użytkownika). Gdy połączenie zostanie zablokowane (Firefox, Safari, odmowa), flasher daje link do
strony samej CLU i zapisuje obraz aplikacji do wybrania tam.

Uwaga: obraz `waveshare`, `f8` i `a8v3` to ten sam układ (`esp32s3`) — CLU nie odróżni płytki,
wgraj obraz właściwy dla swojej.

### Wgrywanie z linii poleceń

Do linii poleceń służy `opengr8on-clu-<płytka>-<wersja>.zip` z wydania (ta sama zawartość co
artefakt z GitHub Actions):

- `opengr8on-clu-<płytka>-full.bin`: pełny obraz od adresu `0x0` (ten sam co `.bin` wydania),
- `opengr8on-clu-<płytka>-ota.bin`: sam obraz aplikacji do aktualizacji przez sieć (ten sam co
  `-ota.bin` wydania),
- osobne obrazy i `flash_args`: aktualizacja bez kasowania ustawień — pełny obraz nadpisuje też
  partycje `nvs` (`0x9000`) i `cfg` (`0x12000`) leżące przed aplikacją,
- `mknvs.py`: obraz ustawień (`cfg.bin`),
- `opengr8on_clu_esp32.elf`: do dekodowania zrzutów awarii tej wersji,
- `CHIP` (`esp32s3` / `esp32`, wartość dla `--chip`) i `VERSION`.

Potrzebny jest Python 3 i:

```bash
pip install "esptool>=5" esp-idf-nvs-partition-gen
```

Port: `/dev/ttyUSB0` / `/dev/ttyACM0` na Linuksie, `/dev/cu.usbserial-…` na macOS, `COM3` na
Windows. Jeśli esptool nie może się połączyć, wejdź w tryb pobierania ręcznie: przytrzymaj
**BOOT** (GPIO0 do masy), naciśnij i puść **RESET**, puść BOOT.

**1. Pierwsza instalacja** (kasuje cały flash, także ustawienia i dysk `a:\`):

```bash
unzip opengr8on-clu-waveshare-v0.0.2-alpha.zip -d clu && cd clu
esptool --chip esp32s3 --port /dev/ttyUSB0 erase-flash
esptool --chip esp32s3 --port /dev/ttyUSB0 write-flash 0x0 opengr8on-clu-waveshare-full.bin
```

**2. Ustawienia** (opcjonalnie): numer seryjny i PIN emulowanej CLU, WiFi, MQTT, w osobnej
partycji `cfg` (`0x12000`, 12 KB). Wartości z niej mają pierwszeństwo przed wkompilowanymi
domyślnymi; pominięte opcje zostają domyślne (seryjny i PIN losowane przy pierwszym starcie,
widoczne w logu UART).

```bash
python mknvs.py --serial 221010155 --pin ABCD1234 \
    --wifi-ssid MojaSiec --wifi-pass 'haslo-wifi' \
    --mqtt-uri mqtt://192.168.1.10:1883 --mqtt-user clu --mqtt-pass 'haslo-mqtt' \
    -o cfg.bin
esptool --chip esp32s3 --port /dev/ttyUSB0 write-flash 0x12000 cfg.bin
```

| Opcja | Klucz (partycja `cfg`) | Opis |
|---|---|---|
| `--serial` | `cfg/sn` (u32) | numer seryjny CLU, dziesiętnie (jak `CLU_SERIAL`) |
| `--pin` | `cfg/pin` | PIN / klucz prywatny, 8 znaków (jak `CLU_PRIVATE_KEY`) |
| `--wifi-ssid`, `--wifi-pass` | `cfg/wifi_ssid`, `cfg/wifi_pass` | sieć WiFi; tylko obraz `waveshare` (płytki z Ethernetem są budowane bez WiFi) |
| `--mqtt-uri` | `cfg/mqtt_uri` | broker, np. `mqtt://192.168.1.10:1883`, `mqtts://host:8883`; puste = MQTT wyłączone |
| `--mqtt-user`, `--mqtt-pass` | `cfg/mqtt_user`, `cfg/mqtt_pass` | dane logowania do brokera |
| `--ota-pass` | `cfg/ota_pass` | hasło aktualizacji przez sieć, 8..64 bajty (zob. [Aktualizacja przez sieć](#aktualizacja-przez-sieć-ota)); pominięte = OTA wyłączone |

Obraz zastępuje tylko partycję `cfg`: stan zapisywany w czasie pracy (partycja `nvs`: klucze
projektu z OM, statyczne IP, lista modułów TF-Bus, czas) zostaje, więc ustawienia można zmieniać w
dowolnym momencie — wgraj nowy obraz i zrestartuj. Gdy numer seryjny albo PIN różni się od
obecnego, CLU go przyjmuje, a klucze projektu wracają do fabrycznych (OM wymaga ponownego
`req_set_key`). Pełny obraz z kroku 1 czyści też `cfg`, więc ustawienia wgrywa się po nim.
`--csv plik.csv` zapisuje też źródłowy CSV (z hasłami jawnym tekstem).

**3. Aktualizacja** z zachowaniem ustawień, stanu i dysku `a:\` (z rozpakowanego katalogu;
wgrywa też aktualną tablicę partycji, adresy aplikacji się nie zmieniają):

```bash
esptool --chip esp32s3 --port /dev/ttyUSB0 write-flash @flash_args
```

Bez Pythona: [flasher w przeglądarce](https://psobiech.github.io/opengr8on-clu-esp32/) (wyżej).

Grenton GATE: `--chip esp32`, w pozostałych przykładach bez zmian.

### Obrazy z GitHub Actions

Workflow **Firmware** buduje każdy push na `develop` / `main` (bez wydania): zakładka **Actions** →
przebieg *Firmware* → **Artifacts**. Zawartość jak `.zip` wydania (zob. wyżej); do flashera wybierz z
niego `…-full.bin` (USB) albo `…-ota.bin` (przez sieć).

### Wymagania do budowania

- **ESP-IDF 6.0** (projekt budowany na 6.0.2, zob. `dependencies.lock`), target `esp32s3` albo
  `esp32` (Grenton GATE).
- Sieć przy pierwszym budowaniu: component manager pobiera `espressif/mqtt`
  (`components/clu_mqtt/idf_component.yml`), `espressif/w5500` i na `esp32` `espressif/lan87xx`
  (`main/idf_component.yml`), bo esp-mqtt i sterowniki Ethernet nie są już częścią ESP-IDF 6.
- Lua jest dołączona w repozytorium (`components/lua`), nic więcej nie trzeba instalować.

### Budowanie

Płytkę wybiera preset CMake (`CMakePresets.json`). Każdy ma własny katalog budowania, target i
plik `sdkconfig`, więc można przełączać się między płytkami bez `set-target` i bez czyszczenia:

| Preset | Płytka | Katalog | Konfiguracja |
|---|---|---|---|
| `waveshare` (domyślny) | [Waveshare ESP32-S3-RS485-CAN](DEVICES.md#waveshare-esp32-s3-rs485-can) | `build/` | `sdkconfig` |
| `gate` | [Grenton GATE](DEVICES.md#grenton-gate) (`esp32`) | `build-gate/` | `sdkconfig.gate` |
| `f8` | [KinCony F8](DEVICES.md#kincony-f8-i-kc868-a8v3) | `build-f8/` | `sdkconfig.f8` |
| `a8v3` | [KinCony KC868-A8v3](DEVICES.md#kincony-f8-i-kc868-a8v3) | `build-a8v3/` | `sdkconfig.a8v3` |

```bash
. /opt/esp-idf/export.sh        # ścieżka do Twojej instalacji ESP-IDF
export IDF_PRESET=f8            # wybór płytki; bez tego: waveshare
idf.py menuconfig               # zob. "Konfiguracja"
idf.py build flash monitor
```

Jednorazowo bez zmiennej: `idf.py --preset gate build`.

### Pierwsze uruchomienie

Log na UART wypisuje `SN=… MAC=… PIN(private key)=…`. Fabryczny klucz projektu jest wyprowadzany z
PIN-u (`docs/UDP.md` › Encryption and keys). Wpisz PIN w Object Managerze (albo w `clu-repl`) przy
pierwszym `req_set_key`. Linia z PIN-em trafia tylko na UART, nie na telnet.

## Konfiguracja

Wszystko w `idf.py menuconfig` → **OpenGr8on ESP32 CLU** (na górze menu).

### Sieć

| Opcja | Domyślnie | Opis |
|---|---|---|
| `CLU_ETH` | y na `esp32`, n na `esp32s3` | Ethernet |
| `CLU_ETH_IF` | EMAC na `esp32`, W5500 na `esp32s3` | wewnętrzny EMAC + PHY RMII (tylko `esp32`, zob. [Grenton GATE](DEVICES.md#grenton-gate)) albo W5500 na SPI (zob. [KinCony F8 i KC868-A8v3](DEVICES.md#kincony-f8-i-kc868-a8v3)) |
| `CLU_ETH_PHY` | LAN87xx | EMAC: sterownik PHY, LAN8720A albo generyczny IEEE 802.3 (np. JL1101) |
| `CLU_ETH_SPI_*_GPIO` | piny F8 | W5500: SCLK / MOSI / MISO / CS / INT (`-1` = odpytywanie co 10 ms) / RST (`-1` = brak) |
| `CLU_ETH_SPI_CLOCK_MHZ` | 20 | W5500: zegar SPI |
| `CLU_WIFI` | y bez Ethernetu, n z nim | WiFi STA; z Ethernetem opcjonalne (oba równolegle) |
| `CLU_WIFI_SSID` / `CLU_WIFI_PASSWORD` | — | sieć WiFi; `cfg/wifi_ssid` / `cfg/wifi_pass` z partycji `cfg` mają pierwszeństwo ([`mknvs.py`](#gotowe-obrazy-github-actions)); bez SSID WiFi jest wyłączone |
| `CLU_WIFI_BSSID` | puste | przypięcie do konkretnego AP (`aa:bb:cc:dd:ee:ff`) |
| `CLU_WIFI_WPA3_ONLY` | n | odrzucaj AP z WPA2 |
| `CLU_WIFI_MAX_TX_POWER` | 0 | moc nadawania w 0,25 dBm, 0 = domyślna sterownika |

Adres IP: DHCP; statyczny ustawia OM przez `req_set_clu_ip` (zapisywany w NVS), osobno dla
każdego interfejsu: zmienia się konfiguracja tego, przez który przyszło żądanie (podsieć nadawcy;
spoza obu podsieci: Ethernet). WiFi: przestrzeń NVS `clu_net` (jak dotąd), Ethernet: `clu_net_eth`;
serwery DNS (cechy 25/26) są wspólne. W odpowiedziach (`resp:<ip>:…`) CLU podaje adres
interfejsu, przez który widzi rozmówcę. Przy obu interfejsach trasa domyślna idzie przez Ethernet.

### Tożsamość CLU

| Opcja | Domyślnie | Opis |
|---|---|---|
| `CLU_SERIAL` | losowy | numer seryjny (dziesiętnie), zapisywany w NVS przy pierwszym starcie |
| `CLU_PRIVATE_KEY` | losowy | PIN / klucz prywatny (8 znaków A–Z0–9) |
| `CLU_IV` | losowy | fabryczny IV projektu (32 znaki hex) |
| `CLU_FW_API_VER` | `0x203` | wersja interfejsu CLU w OM (0x201 = firmware 5.13.x) |
| `CLU_FW_VERSION` | `5.15.24-2635` | wersja raportowana do OM |

Numer seryjny i PIN ustawione w menuconfig zastępują zapisane w NVS; puste = wartości z partycji
`cfg` (gotowe obrazy, `mknvs.py`, zob. [Gotowe obrazy](#gotowe-obrazy-github-actions)), a bez nich
zapisane w NVS albo losowe przy pierwszym starcie.

### TF-Bus / RS485

| Opcja | Domyślnie | Opis |
|---|---|---|
| `CLU_TFBUS` | y | master TF-Bus (moduły na RS485); wyłączony = tylko obiekty wirtualne, skrypty i usługi sieciowe |
| `CLU_RS485_UART` | 1 | numer UART |
| `CLU_RS485_TX_GPIO` / `RX_GPIO` | 17 / 18 | piny |
| `CLU_RS485_DE_GPIO` | 21 | pin DE/RE, `-1` = transceiver z automatycznym kierunkiem |
| `CLU_RS485_BAUD` | 115200 | prędkość magistrali |
| `CLU_LOCAL_IO` | n (y w profilach F8 / A8v3) | 8 przekaźników + 8 wejść płytki na PCF8575 jako IO_MODULE_DIN_8, zob. [DEVICES.md](DEVICES.md#kincony-f8-i-kc868-a8v3) |
| `CLU_LOCAL_IO_SDA_GPIO` / `SCL_GPIO` | 8 / 18 | I2C ekspandera (te same piny co RTC = wspólna magistrala) |
| `CLU_LOCAL_IO_ADDR` | 0x24 | adres PCF8575 |
| `CLU_LOCAL_IO_OUT_PINS` | P10–P17 | przekaźniki na P10–P17 (wejścia P00–P07, F8) albo P00–P07 (wejścia P10–P17, A8v3) |
| `CLU_LOCAL_IO_OUT_ACTIVE_LOW` / `IN_ACTIVE_LOW` | y / y | polaryzacja wyjść / wejść |
| `CLU_LOCAL_IO_SN` | losowy | numer seryjny modułu (dziesiętnie), zapisywany w NVS |
| `CLU_RTC` | y (n na `esp32`) | zegar RTC (brak układu jest logowany i pomijany) |
| `CLU_RTC_CHIP` | PCF85063 | układ: PCF85063 (Waveshare) albo DS3231 (KinCony F8, KC868-A8v3) |
| `CLU_RTC_SDA_GPIO` / `SCL_GPIO` | 39 / 38 | piny I2C zegara RTC (mogą być wspólne z ekspanderem IO) |

### Inne

| Opcja | Domyślnie | Opis |
|---|---|---|
| `CLU_SOFT_RESET` | y | `req_reset` przeładowuje Lua zamiast restartu układu |
| `CLU_SETUP_WINDOW_S` | 60 | okno konfiguracji po uruchomieniu CLU [s]: discovery, zmiana klucza/IP, TFTP (`0` = zawsze) |
| `CLU_SETUP_BUTTON_GPIO` | 0 (BOOT) | przycisk otwierający okno konfiguracji w trakcie pracy, `-1` = brak |
| `CLU_LUA_5_3` / `CLU_LUA_5_5` | 5.3 | wersja Lua (5.3 jak w CLU, 5.5 eksperymentalnie) |
| `CLU_LUA_MEM_LIMIT_KB` | 4096 | limit sterty Lua (KB) |
| `CLU_LUA_INTERNAL_RESERVE_KB` | 64 | wewnętrzny RAM zostawiany dla systemu |
| `CLU_TELNET` | y | konsola logów |
| `CLU_TELNET_PORT` / `MAX_CLIENTS` / `HISTORY` | 23 / 2 / 8192 | port, liczba klientów, bufor historii |
| `CLU_TELNET_ALLOW` | puste (wszyscy) | dozwolone adresy: IP lub CIDR |
| `CLU_OTA` | y | aktualizacja przez sieć ([OTA](#aktualizacja-przez-sieć-ota)) |
| `CLU_OTA_PASSWORD` | puste (wyłączone) | hasło OTA, 8..64 znaki; `cfg/ota_pass` z partycji `cfg` ma pierwszeństwo |
| `CLU_OTA_PORT` | 8080 | port HTTP (strona aktualizacji pod `/`) |
| `CLU_OTA_CORS_ORIGIN` | `https://psobiech.github.io` | strona, która może wywołać `POST /ota` z przeglądarki (CORS); puste = tylko strona CLU |

### Projekt

Projekt wgrywa się normalnie z Object Managera (OM.LUA, USER.LUA przez TFTP). Moduły TF-Bus
wykrywa się w OM przyciskiem odświeżania modułów; lista trafia do `a:\CONFIG.JSON`.

Listą modułów zarządza CLU: każdy moduł wykryty na magistrali dostaje najniższy wolny adres,
zapamiętany w NVS razem z numerem seryjnym, więc adresy nie zmieniają się po restarcie.
Magistrala działa od startu, także bez MAIN.LUA / OM.LUA (wykrywanie, odpytywanie,
`tfbusDevices`); OM.LUA tylko przypisuje modułom nazwy po numerze seryjnym, a inicjalizacja
modułów (sterowniki) rusza po `SYSTEM.Init`. Moduł z OM.LUA, którego CLU nie zna, jest
dopisywany do listy. Moduły znikają z listy tylko przy odświeżeniu modułów
(`req_refresh_modules`), gdy nie ma ich na magistrali; moduł chwilowo offline zostaje. Usunięty
moduł zachowuje jednak swój adres: gdy wróci, dostaje ten sam, a nowe moduły zajmują go dopiero,
gdy nie ma już innych wolnych adresów.

Zamiast Object Managera można użyć rozszerzenia VS Code
[opengr8on-vscode](https://github.com/psobiech/opengr8on-vscode): projekt jako pliki YAML + Lua
w gicie, z których generuje te same OM.LUA / USER.LUA co Object Manager i wysyła je przez TFTP;
wykrywa CLU, pokazuje moduły, wartości na żywo i logi (telnet).

## MQTT / Home Assistant

### Włączenie

`idf.py menuconfig` → **OpenGr8on ESP32 CLU** → **OpenGr8on MQTT / Home Assistant**:

| Opcja | Domyślnie | Opis |
|---|---|---|
| `CLU_MQTT_BROKER_URI` | puste = wyłączone | np. `mqtt://192.168.1.10`, `mqtts://broker:8883`; `cfg/mqtt_uri` z partycji `cfg` ma pierwszeństwo ([`mknvs.py`](#gotowe-obrazy-github-actions)) |
| `CLU_MQTT_USERNAME` / `PASSWORD` | anonimowo | `cfg/mqtt_user` / `cfg/mqtt_pass` z partycji `cfg` mają pierwszeństwo |
| `CLU_MQTT_CLIENT_ID` | `opengr8on-CLU<serial>` | |
| `CLU_MQTT_TOPIC_BASE` | `grenton` | |
| `CLU_MQTT_KEEPALIVE` | 30 s | |
| `CLU_MQTT_SCAN_MS` | 100 ms | interwał skanowania stanu obiektów |
| `CLU_MQTT_HA_DISCOVERY` | y | discovery Home Assistant |
| `CLU_MQTT_HA_PREFIX` | `homeassistant` | prefiks discovery |
| `CLU_MQTT_DIAGNOSTICS` | y | encje diagnostyczne firmware |
| `CLU_MQTT_DIAG_INTERVAL` | 30 s | próbkowanie diagnostyki |
| `CLU_MQTT_OBJECT_API` | **n** | surowe API obiektów (zob. niżej) |

Klient łączy się, gdy CLU dostanie adres IP (bez czekania na skrypty Lua), o ile cecha lokalnej
CLU **UseCloud (18)** = 1 — domyślnie w OM; OM.LUA zapisuje `CLUxxx:set(18, 1)`. Discovery Home
Assistanta jest wysyłane po `SYSTEM.Init` (po OM.LUA). `CLUxxx:set(18, 0)` (z OM, `req:` lub skryptu)
publikuje `offline` i rozłącza, `set(18, 1)` łączy ponownie. `CLUxxx:get(19)` (CloudConnection) = 1,
gdy broker jest połączony.

**Brak rejestracji w chmurze Grentona.** Przy UseCloud=1 OM (1.12.1) zwykle czyta
`a:\CLOUD-PUBLIC.CSR`, zleca podpisanie chmurze Grentona i wgrywa `CLOUD-PUBLIC.CRT` /
`CLOUD-ROOT.PEM` — ale tylko gdy `a:\CLOUD-PUBLIC.CRT` nie istnieje lub jest pusty. Firmware przy
starcie zapisuje niepusty zastępczy `CLOUD-PUBLIC.CRT`, więc OM pomija ten krok.

**TLS** (`mqtts://`, `wss://`): CA z `a:\MQTT_CA.PEM`, jeśli istnieje (wgranie przez TFTP), w
przeciwnym razie wbudowany pakiet publicznych CA.

### Home Assistant

1. Zainstaluj broker (np. dodatek Mosquitto) i integrację MQTT w HA z włączonym discovery.
2. Utwórz użytkownika brokera dla CLU, wpisz URI i dane w menuconfig, zbuduj i wgraj.
3. Wgraj projekt z OM. Po połączeniu CLU pojawia się w HA jako jedno urządzenie nazwane według
   `-- NAME_CLU` z OM.LUA; encje mają nazwy z komentarzy `-- NAME_IO`.

Discovery (`homeassistant/<component>/opengr8on_CLU<serial>/<obj>/config`, retained) jest wysyłane
przy każdym połączeniu i gdy HA opublikuje `online` na `homeassistant/status`. Konfiguracje encji,
które zniknęły po zmianie OM.LUA, są czyszczone (lista w `a:\STORAGE.DAT`).

| Klasa | Encja HA | Polecenia |
|---|---|---|
| DOUT (4) | `switch` | ON/OFF → SwitchOn/SwitchOff |
| DIN (3) | `binary_sensor` + `event` | — |
| BUTTON (7), PANEL_BUTTON (30) | `event` (press, release, short_press, long_press, hold, click) | — |
| ROLLER_SHUTTER (24) | `cover` (0 zamknięta … 100 otwarta) | OPEN/CLOSE → HoldUp/HoldDown, STOP, pozycja → SetPosition |
| PANELSENSTEMP (20) / PANELSENSLIGHT (21) | `sensor` °C / % | — |
| lokalna CLU (0)¹ | `sensor` czas startu i stan (starting/ok/emergency), `event` time_change | — |
| moduł TF-Bus (2)¹ | `binary_sensor` connectivity | — |
| firmware (`system`)¹ | `sensor` heap / min. heap / PSRAM / pamięć Lua (KiB), RSSI, IP, przyczyna resetu, moduły online / łącznie / pominięte odpytania; `button` Restart | — |

¹ `CLU_MQTT_DIAGNOSTICS`, sekcja „Diagnostic” w HA. Heap, PSRAM, pamięć Lua, RSSI i pominięte
odpytania próbkowane co `CLU_MQTT_DIAG_INTERVAL` s, reszta przy zwykłym skanowaniu; publikowane
tylko przy zmianie.

### Tematy

`<base>` = `grenton/CLU<serial>`.

```
<base>/status                                  online/offline (retained, LWT)
<base>/<obj>/state                             retained, przy zmianie
<base>/<obj>/set                               polecenie
<base>/<obj>/position | position/set           rolety
<base>/<obj>/event                             {"event_type":"click"} (nie retained)

<base>/system/clu/boot_time | state | reset_reason     ISO time | starting/ok/emergency | text
<base>/system/clu/event                                {"event_type":"time_change"}
<base>/system/clu/restart/set                          PRESS -> reboot
<base>/system/memory/heap_free | heap_min | psram_free | lua     KiB
<base>/system/network/rssi | ip                        dBm | text
<base>/system/tfbus/online | total | missed_polls      counters
<base>/system/tfbus/modules/<module>                   ON/OFF
```

### Surowe API obiektów

Tylko z `CLU_MQTT_OBJECT_API=y`. Dowolna cecha / metoda dowolnego obiektu z OM.LUA (w tym lokalnej
CLU); ładunki to wartości JSON, odpowiedzi nie są retained.

| Publikuj | Odpowiedź na | Przykład odpowiedzi |
|---|---|---|
| `<base>/<OBJ>/feature/<idx>/get` (dowolny ładunek) | `<base>/<OBJ>/feature/<idx>` | `{"ok":true,"value":0}` |
| `<base>/<OBJ>/feature/<idx>/set` `2500` / `"txt"` / `true` / zwykły tekst | `<base>/<OBJ>/feature/<idx>` | odczytana wartość |
| `<base>/<OBJ>/execute/<idx>` `[args…]` / pojedyncza wartość / pusto | `<base>/<OBJ>/execute/<idx>/result` | `{"ok":true,"value":…}` / `{"ok":false,"error":"…"}` |

Proxy zdalnych CLU (klasa 1) są odrzucane (ich `execute(0, s)` uruchamia Lua na drugiej CLU).

### Nadpisania w USER.LUA

Przed lub po OM.LUA, także z OnInit:

```lua
MQTT.expose(DOU1529, "light")                        -- replace the default mapping
MQTT.expose("TIM0001", { kind = "sensor", feature = 3, unit = "ms", name = "Countdown" })
MQTT.expose(TIM0001, { kind = "button", execute = 0, name = "Start timer" })
MQTT.hide(DOU7295)
MQTT.hide("system")                                  -- no firmware entities
MQTT.publish("custom/topic", "payload", true)        -- retained; MQTT.connected(), MQTT.root
```

- Rodzaje: `switch`, `light`, `binary_sensor`, `sensor`, `cover`, `event`, `button`, `module`.
- Opcje: `name`, `feature`, `unit`, `device_class`, `state_class`, `icon`, `entity_category`,
  `events`; czujniki także `text = true` (stan tekstowy), `map = {[v] = "label"}` (enum); przyciski
  `execute = idx` (lub `press = function(o) … end`).

## Testy

Testy działają na hoście (Linux), bez ESP-IDF i bez sprzętu. Kompilują właściwe źródła firmware z
zaślepkami FreeRTOS/ESP (`test/host/stubs`), z ASan i UBSan.

Wymagania: `gcc`/`clang`, `make`, interpreter `lua5.3` (i `lua5.5` dla testów 5.5); dla
`test_crypto` mbedtls 3.6 z API PSA (Arch: pakiet `mbedtls3`; gdzie indziej ustaw `MBEDTLS3_INC` /
`MBEDTLS3_LIB`) — bez niego test jest pomijany.

```bash
make -C test/host test                            # wszystko poniżej
make -C test/host test-lua OM="/path/to/OM.LUA"   # dodatkowo wczytaj prawdziwy OM.LUA przez prelude
make -C test/host LUA=5.5 clean test              # to samo na Lua 5.5
make -C test/host coverage                        # pokrycie C (gcov) i Lua; FAIL_UNDER=<%> = próg
```

| Test | Co sprawdza |
|---|---|
| `test_proto` | ramkowanie TF-Bus, wyprowadzanie kluczy, TFTP |
| `test_crypto` | AES i klucze vs. wektory z opengr8on-node |
| `test_drivers` | sekwencje sterowników bajt po bajcie vs. przechwyt prawdziwej CLU |
| `test_master` | master magistrali na symulowanej magistrali (adresowanie, discovery, odpytywanie) |
| `test_prelude.lua` | model obiektów Lua, obiekty wirtualne, OM.LUA z `fixtures/` |
| `test/web/test_nvs.mjs` | obraz ustawień z flashera (`web/nvs.js`) bajt w bajt jak `tools/mknvs.py` (`node test/web/test_nvs.mjs`) |
| `test/web/test_ota.mjs` | aktualizacja przez sieć we flasherze (`web/ota.js`): wycinanie obrazu aplikacji z pełnego obrazu, adres CLU, Basic auth |
| `test_mqtt.lua` | mapowanie encji HA, tematy, surowe API |
| `test_udp` | serwer UDP (`main/udp_server.c`): discovery, klucze, `req:`, TFTP/diagnostyka, reset, wywołania zdalnych CLU, okno konfiguracji i przycisk |
| `test_uart` | transport RS485 (`tfbus_uart.c`) na symulowanym sterowniku UART: echo, odpowiedzi dzielone, timeouty, lokalne moduły |
| `test_localio` | emulacja IO_MODULE_DIN_8 (`localio.c`): DISCOVER, adresowanie, odpytywanie, ACTIVATE/DEACTIVATE, debouncing, awarie ekspandera, RESET |
| `test_localio_f8` / `test_localio_a8v3` | IO płytki (`clu_localio.c`) na symulowanym PCF8575: mapowanie pinów F8 / A8v3, numer seryjny w NVS |
| `test_sys` | `main/`: statyczne IP i DNS, strefa czasu, SNTP i odtwarzanie czasu z RTC, tożsamość i klucze (NVS), CONFIG.JSON / config.txt, pakiet diagnostyczny, `sys_info` |
| `test_tftp` | serwer TFTP: pobieranie / wysyłanie (octet, netascii), utrata pakietów, obce hosty, limity, błędy |
| `test_telnet` | konsola logów: historia, filtr `CLU_TELNET_ALLOW`, komendy telnet (IAC, Ctrl-C), wolni klienci |
| `test_mqttc` | połączenie MQTT (`clu_mqtt.c`): konfiguracja klienta i TLS, subskrypcje, wiadomości przychodzące, API Lua |
| `test_lua` | środowisko Lua (`clu_lua.c`): wywołania `req:`, limit czasu, zdarzenia, przeładowanie, tryb awaryjny, `BAD_M.LUA` |
| `test_rtc` | RTC PCF85063 i DS3231: kodowanie BCD ↔ epoch (lata przestępne, zakres 2000–2099, flagi OS / OSF, tryb 12h), sterownik I2C na symulowanych układach, wspólna magistrala I2C |
| `test_main` | kolejność startu, konfiguracja modułów, zdarzenia WiFi, mapowanie cech CLU (NTP, strefa, DNS, UseCloud) |

`make coverage` zapisuje też `test/host/coverage/summary.md` (tabela Markdown) i `coverage.json`;
wymaga tylko `gcc`/`gcov`, `python3` i `luac$(LUA)`.

## Bezpieczeństwo

### Wzmocnienia

- **Lua uruchamia tylko tekst:** `load`/`loadfile`/`dofile`/`require` i skrypty startowe odrzucają
  prekompilowany bytecode (niezweryfikowany bytecode = dowolny dostęp do pamięci). `string.dump` i
  `package.loadlib` są usunięte, z `debug` zostaje tylko `traceback`.
- **Stos i sterta:** limit rekursji C w Lua 120 (`components/lua/CMakeLists.txt`) przy stosie
  zadania Lua 48 KB; stack protector, watchpoint końca stosu i lekkie zatruwanie sterty
  (`sdkconfig.defaults`). Limit sterty Lua (`CLU_LUA_MEM_LIMIT_KB`, domyślnie 4 MB).
- **Limit czasu `req:`** 2,5 s — zapętlone skrypty nie zawieszają środowiska. Limit liczy tylko
  instrukcje Lua; długie wywołania C (np. `string.find` z backtrackingiem) nie są przerywane.
- **Brak wyroczni paddingu CBC:** pakiety, które dają się odszyfrować, ale nie są poleceniem
  `req…`, nie dostają odpowiedzi.
- **Okno konfiguracji po starcie** (`CLU_SETUP_WINDOW_S`, domyślnie 60 s, `0` wyłącza): tylko przez
  tyle sekund od pełnego uruchomienia CLU po starcie układu (zasilanie, przycisk reset, restart;
  odliczanie rusza, gdy WiFi po raz pierwszy dostanie IP) albo od
  wciśnięcia przycisku konfiguracji (`CLU_SETUP_BUTTON_GPIO`, domyślnie BOOT/GPIO0, przytrzymanie ≥100 ms
  w trakcie pracy — przytrzymany podczas resetu włącza tryb wgrywania), a w trybie awaryjnym
  (MAIN.LUA zawiódł, działa EMERGNCY.LUA) bez limitu czasu, aż CLU z niego wyjdzie — CLU odpowiada na
  discovery (`req_discovery_clu`), przyjmuje `req_set_key`, `req_set_clu_ip`, `req_start_ftp` i
  `req_diagnostic_refresh` oraz w ogóle próbuje klucza PIN/tymczasowego. Później discovery i
  broadcastowy `req_set_clu_ip` są ignorowane bez odpowiedzi, pozostałe dostają `resp:ERROR`,
  a pakiety pod kluczem PIN nie są odszyfrowywane. Zwykła praca pod kluczem projektu (`req:`,
  `req_reset`, `req_refresh_modules`, `req_tftp_stop`) działa bez zmian. Sesja TFTP otwarta w
  oknie trwa do `req_tftp_stop` lub limitu bezczynności. Discovery odebrane w oknie zatrzymuje je
  otwarte (bez limitu czasu) aż do najbliższego `req_reset` (miękkiego lub twardego) albo restartu
  układu. Miękki reset (`req_reset` przy
  `CLU_SOFT_RESET`) **nie** otwiera okna ponownie — pakiet da się odtworzyć (stały IV). **Wpływ na
  OM:** przed wyszukiwaniem CLU, ustawieniem klucza projektu lub wgrywaniem projektu wciśnij
  przycisk BOOT (lub zresetuj płytkę) i zrób to w ciągu tego czasu. Otwarcie i zamknięcie okna jest logowane.
- **TFTP:** obsługuje tylko hosta, który wysłał `req_start_ftp`, wyłącza się po 5 min
  bezczynności, wysyłane pliki do 2 MB.
- **PIN-y:** nowe losowe PIN-y z alfabetu A–Z0–9 (~41 bitów).
- **Telnet bez uwierzytelniania:** ogranicz przez `CLU_TELNET_ALLOW` (IP lub CIDR; błędna wartość
  odrzuca wszystkich) albo wyłącz `CLU_TELNET`. Linia z PIN-em jest logowana przed instalacją
  hooka, a przy każdym discovery PIN trafia na `stderr` z pominięciem `ESP_LOG`, więc zostaje
  tylko na UART; hasło WiFi nie jest logowane. Skrypty Lua nadal mogą wypisać
  cokolwiek przez `print`.
- **MQTT nigdy nie wykonuje Lua:** każdy temat odpowiada stałemu wywołaniu obiektu, a JSON surowego
  API jest dekodowany, nigdy nie przechodzi przez `load`. Mimo to przy włączonym
  `CLU_MQTT_OBJECT_API` każdy, kto może publikować pod `<base>/`, steruje każdym obiektem —
  **ogranicz to przez ACL brokera** i używaj osobnego użytkownika dla CLU.
- **OTA tylko z hasłem:** bez hasła (`cfg/ota_pass` / `CLU_OTA_PASSWORD`) serwer HTTP nie startuje.
  Hasło jest porównywane w stałym czasie, a każda błędna próba kosztuje ~2 s (jedno żądanie naraz).
  To zwykły HTTP z Basic auth — **hasło idzie przez sieć jawnym tekstem**, więc aktualizuj z
  zaufanej sieci lokalnej i nie używaj tego hasła nigdzie indziej. Firmware nie jest podpisany:
  kto zna hasło, może wgrać dowolny obraz z tą samą nazwą projektu.
  CORS (`CLU_OTA_CORS_ORIGIN`) wpuszcza tylko jedną stronę i nie zastępuje hasła; puste = wyłączone.
- **Brak połączeń z chmurą Grentona** (zastępczy `CLOUD-PUBLIC.CRT`).

### Ograniczenia wynikające z protokołu Grentona

Nie do naprawienia bez utraty zgodności z OM:

- klucz projektu używa stałego IV, więc przechwycone polecenia można odtworzyć (replay);
- `req_set_clu_ip` wymaga tylko publicznego klucza rozgłoszeniowego i numeru seryjnego (z
  discovery) — ograniczone do okna konfiguracji po starcie (`CLU_SETUP_WINDOW_S`);
- clientRegister może skierować raporty na dowolny adres.

**Trzymaj CLU w zaufanym, odseparowanym segmencie sieci.**

### Dostęp fizyczny

Urządzenie jest otwarte: właściciel może je swobodnie przeprogramować i zmieniać kod. Flash nie
jest szyfrowany, więc klucze, PIN, hasło WiFi i hasło MQTT można odczytać przez USB (esptool).
Chroń fizyczny dostęp do płytki tak samo jak do szafy rozdzielczej.

## Licencje

Kod źródłowy projektu jest objęty licencją [AGPLv3](LICENSE.txt), z wyjątkiem komponentów
zewnętrznych wymienionych niżej.

Lua (`components/lua`) jest objęta licencją MIT (`components/lua/LICENSE`). Komponenty pobierane
przez component manager (`espressif/mqtt`) i ESP-IDF mają własne licencje.

Opisy protokołów, na których opiera się firmware, pochodzą z
[opengr8on-node](https://github.com/psobiech/opengr8on-node) (dokumentacja: CC BY-SA 4.0).

### Object Manager

Repozytorium nie zawiera ani nie rozpowszechnia Object Manager'a, firmware CLU ani innych plików
firmy Grenton. Object Manager jest objęty
[licencją użytkownika końcowego Grentona](https://grenton.pl/om_eula_v1/), która wiąże licencję OM
z zakupionymi urządzeniami Grenton. Użycie OM z CLU działającą na tym firmware może wykraczać poza zakres tej licencji — oceń to samodzielnie przed użyciem. Projekt ten nie reklamuje ani nie zachęca do
używania OM z tym oprogramowaniem.

## Zastrzeżenie prawne

Projekt nie jest popierany przez, bezpośrednio powiązany z, utrzymywany, autoryzowany ani
sponsorowany przez Grenton Sp. z o.o.

Użycie jakichkolwiek nazw handlowych lub znaków towarowych służy wyłącznie celom identyfikacyjnym i
referencyjnym i nie implikuje żadnego związku z właścicielem znaku towarowego ani jego marką
produktową.

Wszelkie nazwy produktów, logotypy, marki i inne znaki towarowe lub obrazy wymienione na tej stronie
stanowią własność ich odpowiednich właścicieli.

Korzystanie z tego oprogramowania — w tym podłączanie urządzenia z tym firmware do magistrali
TF-Bus i modułów Grenton — może naruszać warunki gwarancji urządzeń Grenton. Użytkownik ponosi
wyłączną odpowiedzialność za ocenę zgodności z warunkami gwarancji producenta przed użyciem tego
oprogramowania z urządzeniami objętymi gwarancją.

Firmware steruje fizycznymi urządzeniami (przekaźniki, rolety, oświetlenie). Błędy w
oprogramowaniu, sprzęcie lub konfiguracji mogą spowodować nieoczekiwane załączenie lub wyłączenie
obwodów. Nie używaj go do sterowania obwodami, od których zależy bezpieczeństwo ludzi lub mienia.
Instalację elektryczną powinna wykonywać osoba z odpowiednimi uprawnieniami.

Oprogramowanie jest dostarczane w stanie „takim, jaki jest” (as-is), bez jakichkolwiek gwarancji,
wyraźnych ani dorozumianych, w tym między innymi bez gwarancji przydatności handlowej, przydatności
do określonego celu ani nienaruszania praw osób trzecich. O ile wyraźnie nie zaznaczono inaczej,
osoba która powiązała utwór z niniejszą adnotacją nie udziela żadnych gwarancji dotyczących tego
utworu i zrzeka się odpowiedzialności za wszelkie sposoby jego użycia, w najszerszym zakresie
dopuszczalnym przez obowiązujące prawo.

---

### Licenses

Project source code is licensed under [AGPLv3](LICENSE.txt), except for the third-party components
listed below.

Lua (`components/lua`) is MIT-licensed (`components/lua/LICENSE`). Components fetched by the
component manager (`espressif/mqtt`) and ESP-IDF carry their own licenses.

Protocol descriptions the firmware is based on come from
[opengr8on-node](https://github.com/psobiech/opengr8on-node) (documentation: CC BY-SA 4.0).

#### Object Manager

This repository does not contain or distribute Object Manager, CLU firmware or any other Grenton
files. Object Manager is covered by
[Grenton's end-user license agreement](https://grenton.pl/om_eula_v1/), which ties the OM license
to purchased Grenton devices. Using OM with a CLU running this firmware may fall outside the scope
of that license — evaluate this yourself before use. Tested OM version: 1.13.x. This project does
not advertise or encourage using OM with this software.

### Disclaimer

This project is not endorsed by, directly affiliated with, maintained, authorized, or sponsored by
Grenton Sp. z o.o.

The use of any trade name or trademark is for identification and reference purposes only and does
not imply any association with the trademark holder of their product brand.

Any product names, logos, brands, and other trademarks or images featured or referred to within this
page are the property of their respective trademark holders.

Use of this software — including connecting a device running this firmware to the TF-Bus and to
Grenton modules — may void the warranty of your Grenton devices. It is solely your responsibility to
evaluate whether using this software with warranted hardware complies with the manufacturer's
warranty terms.

This firmware controls physical equipment (relays, roller shutters, lighting). Bugs in the software,
hardware or configuration may switch circuits on or off unexpectedly. Do not use it to control
circuits on which the safety of people or property depends. Electrical installation work should be
done by a qualified person.

This software is provided "as-is", without warranty of any kind, express or implied, including but
not limited to the warranties of merchantability, fitness for a particular purpose, and
non-infringement. Unless expressly stated otherwise, the person who associated a work with this deed
makes no warranties about the work, and disclaims liability for all uses of the work, to the fullest
extent permitted by applicable law.
