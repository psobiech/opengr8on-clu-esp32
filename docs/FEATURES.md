# Funkcje

Lista funkcji firmware, pogrupowana tematycznie. Szczegóły, opcje i stan weryfikacji: linki w
każdej grupie. Stan względem prawdziwej CLU (✅ / 🟡 / ❌):
[Zgodność z CLU_ZWAVE_2](../README.md#zgodność-z-clu_zwave_2).

- [Sterowniki modułów TF-Bus](#sterowniki-modułów-tf-bus)
- [IO na płytce](#io-na-płytce)
- [MySensors](#mysensors)
- [GATE HTTP](#gate-http)
- [MQTT / Home Assistant](#mqtt--home-assistant)
- [Sieć](#sieć)
- [Czas](#czas)
- [Diagnostyka i logi](#diagnostyka-i-logi)
- [Instalacja i aktualizacja](#instalacja-i-aktualizacja)
- [Zgodność z Object Managerem](#zgodność-z-object-managerem)
- [Lua i obiekty wirtualne](#lua-i-obiekty-wirtualne)
- [Magistrala TF-Bus](#magistrala-tf-bus)
- [Bezpieczeństwo](#bezpieczeństwo)
- [Sprzęt](#sprzęt)
- [Testy](#testy)

## Sterowniki modułów TF-Bus

Szczegóły: [Moduły TF-Bus](../README.md#moduły-tf-bus), [TFBUS.md](TFBUS.md).

- **IO_MODULE_DIN_8** (0x1e): 8× DOUT, 8× DIN, PowerSupplyVoltage; czasowe Switch*, click / hold
  ze zboczy wejść.
- **RELAY_DIN_4** (0x15): 4× DOUT, PowerSupplyVoltage.
- **ROLLER_SH_DIN_3** (0x2a): 3× ROLLER_SHUTTER (czasy góra / dół, pozycja), PowerSupplyVoltage.
- **ROLLER_SH_DIN** (0x17, fwApiVer 1): 1× ROLLER_SHUTTER ze starymi indeksami cech.
- **TOUCH_PANEL_FM_4** (0x1c): 4× BUTTON, PANELSENSTEMP, PANELSENSLIGHT (próg, czułość, zdarzenia
  z histerezą), PANEL (buzzer), PowerSupplyVoltage.
- **SMART_PANEL_FM_4** (0x03): 16× PANEL_BUTTON (LED, etykieta, ikony A / B, tryb
  monostabilny / bistabilny / zablokowany), PANEL (gesty i ich ikony, zbliżenie, rysowanie
  PrintText / PrintFloat / DrawLine / DrawPoint / DrawBox / DrawIcon, strony SetPageNr / Next /
  Prev, Beep, włączenie / wyłączenie ekranu), 4× PANEL_PAGE, PANELSENSTEMP, PANELSENSLIGHT.
- **DIMMER_MOSFET_DIN** (0x1a): DIMM: SetValue / Switch* / Hold z rampą, StartLevel,
  DistributedLogicGroup, Min / MaxValue.
- **LED_RGBW_DIN** (0x18): LEDRGB (kolor RGBW, korekcje Red / Green / Blue, rampa, Hold*),
  4× LED_CHANNEL, 2× LED_CCT (temperatura barwowa), PowerSupplyVoltage.
- **ANALOG_DIN** (0x19) i **ONE_WIRE** (0xff), wyłącznie emulowane przez IO płytki: AnalogIN
  (Value = RawValue × Scale, Value %, czułość, zakres, zdarzenia), AnalogOUT (bez sprzętu),
  ONE_WIRE_HUB (RefreshSensors, liczniki czujników), ONEW_SENSOR (°C, próg, status, numer ROM).
- Wspólne funkcje: PowerSupplyVoltage (klasa 12), timery Switch* i kierunek Hold* modułów
  ściemnialnych.

## IO na płytce

Szczegóły: [IO na płytce, RTC, wyświetlacz](../README.md#io-na-płytce-rtc-wyświetlacz),
[KinCony F8](devices/kincony-f8.md), [KC868-A8v3](devices/kincony-kc868-a8v3.md).

- Przekaźniki i wejścia płytki (PCF8575 na I2C) jako emulowany moduł TF-Bus **IO_MODULE_DIN_8**,
  widoczny w OM jak prawdziwy; z eliminacją drgań styków (debouncingiem) i obsługą awarii
  ekspandera.
- Wejścia GPIO jako drugi moduł IO_MODULE_DIN_8.
- Wejścia analogowe (ADC1, współczynnik na pin) jako AnalogIN modułu **ANALOG_DIN**.
- Czujniki 1-Wire DS18B20 / DS18S20 (do 4 magistral) jako moduły **ONE_WIRE**.
- Działa z każdym trybem RS485, także bez magistrali (`CLU_BUS_NONE`).
- Zegar RTC z baterią: PCF85063 albo DS3231.
- Ekran statusu na wyświetlaczu OLED SSD1306 128×64: numer seryjny, adresy IP
  (Ethernet / WiFi), czas, liczba modułów online, czas pracy.
- Wspólna magistrala I2C dla ekspandera, RTC i wyświetlacza.
- Przycisk konfiguracji (domyślnie BOOT) otwierający okno konfiguracji.

## MySensors

Szczegóły: [MYSENSORS.md](MYSENSORS.md).

- Bramka sieci MySensors na RS485 (`MY_RS485`) zamiast TF-Bus (opcja kompilacji, osobne obrazy).
- Kontroler w firmware: przydział identyfikatorów węzłów (NVS), `I_CONFIG`, `I_TIME`, odpowiedzi
  na `C_REQ`, rejestr węzłów (szkic, wersja, bateria).
- Węzły z `MY_NODE_ID` = AUTO.
- Czujniki binarne i wyjścia jako kanały emulowanych modułów IO_MODULE_DIN_8 w OM.
- API `MySensors.*` w USER.LUA: `on`, `onMessage`, `send`, `request`, `internal`, `get`, `nodes`,
  `status`, `trace`, `slots`, `unmap`.
- Encje Home Assistant dla modułów węzłów.

## GATE HTTP

Szczegóły: [GATE_HTTP.md](GATE_HTTP.md).

- Emulacja Grenton GATE HTTP dla OM (zamiast CLU, bez magistrali).
- **HttpRequest** (klasa 121): wywołania HTTP / HTTPS z Lua.
- **HttpListener** (klasa 120): serwer HTTP (domyślnie port 80), odpowiedź ze skryptu
  (`SendResponse`), limit czasu → 504.
- Typy treści jak w prawdziwym GATE; skrypty z instrukcji GATE HTTP działają bez zmian.
- Osobne obrazy dla ESP32-S3 16 MB i 8 MB flash (N8R8).

## MQTT / Home Assistant

Szczegóły: [MQTT / Home Assistant](../README.md#mqtt--home-assistant).

- Klient MQTT (`mqtt://`, `mqtts://`, `ws://`, `wss://`), TLS z certyfikatem CA z
  `a:\MQTT_CA.PEM` albo z wbudowanym pakietem CA; LWT `online` / `offline`.
- Włączanie / wyłączanie z OM przez cechę UseCloud (18), stan połączenia w CloudConnection (19).
- Discovery Home Assistant dla każdego urządzenia: CLU, każdy moduł TF-Bus jako osobne
  urządzenie, przyciski / ekrany paneli i kanały LED jako podurządzenia; nazwy z OM.LUA.
- Encje dla: DOUT, DIN, DIMM, LED_CHANNEL, LED_CCT, LEDRGB, BUTTON, PANEL_BUTTON, PANEL
  (rysowanie na SMART_PANEL), ROLLER_SHUTTER, czujników paneli, AnalogIN, ONEW_SENSOR,
  ONE_WIRE_HUB, PowerSupplyVoltage, Calendar, SunriseSunsetCalendar, Statistics, cech
  użytkownika CLU.
- Dostępność encji modułu zależna od stanu jego łącza na magistrali.
- Automatyczne usuwanie z HA encji, które zniknęły z projektu.
- Encje diagnostyczne: wersja firmware, heap / PSRAM / pamięć Lua, obciążenie poszczególnych
  rdzeni CPU, RSSI, IP, przyczyna resetu, ostatnia awaria, stan CLU, liczniki i obciążenie TF-Bus,
  przycisk Restart.
- Wysyłanie logów na temat MQTT (`CLU_MQTT_LOG`, opcjonalnie) z buforowaniem na czas rozłączenia.
- Surowe API obiektów (`CLU_MQTT_OBJECT_API`, opcjonalnie): get / set dowolnej cechy, wywołanie
  dowolnej metody.
- API `MQTT.*` w USER.LUA: `expose` (własne mapowania encji), `hide`, `publish`, `connected`.

## Sieć

Szczegóły: [Sieć](../README.md#sieć).

- WiFi STA: przypięcie do BSSID, tryb tylko WPA3, limit mocy nadawania.
- Ethernet: wewnętrzny EMAC + PHY RMII (LAN8720A, generyczny IEEE 802.3, np. JL1101) na `esp32`
  albo W5500 na SPI.
- WiFi i Ethernet równolegle; trasa domyślna przez Ethernet; odpowiedzi wysyłane z adresu
  interfejsu rozmówcy.
- DHCP albo statyczny adres IP dla każdego interfejsu (z OM).
- Okresowe logowanie diagnostyki sieci (`CLU_NET_DIAG_INTERVAL`).

## Czas

- SNTP (serwer z cechy 22 CLU), strefa czasowa (cecha 23, mapowana na reguły POSIX TZ z czasem
  letnim), zapisywane w NVS, więc czas lokalny jest poprawny już przed OnInit.
- Ustawienie zegara z RTC przy starcie, przed Lua (skrypty mają czas bez sieci); każda
  synchronizacja SNTP oraz wywołanie `SetDateTime` zapisują czas do RTC.
- Zdarzenie OnTimeChange.

## Diagnostyka i logi

- Konsola logów przez telnet (TCP 23, tylko odczyt): historia, kilku klientów, lista dozwolonych
  adresów (IP / CIDR), zamykanie sesji skrótem Ctrl-C / Ctrl-D.
- Symulowane moduły w `CONFIG.JSON` / `config.txt` do testów w OM (`CLU_DEBUG_EXTRA_MODULES`).
- `__clu.sys_info()` w Lua: heap, PSRAM, sieć, przyczyna resetu, obciążenie CPU, liczniki TF-Bus i
  UART.
- Zachowywanie przyczyny ostatniej awarii (wyjątek, abort, watchdog: zadanie, PC) pomiędzy
  restartami w pamięci RTC; wykrywanie pętli restartów wywołanej skryptem.
- Pakiet diagnostyczny dla OM (`DIAGNOSTIC_PACK.JSON`).
- Log UART z numerem seryjnym, MAC i PIN-em przy starcie.

## Instalacja i aktualizacja

Szczegóły: [Budowanie i wgrywanie](../README.md#budowanie-i-wgrywanie).

- Gotowe obrazy dla każdej płytki w GitHub Releases (pełny `.bin`, `-ota.bin`, `.zip` do wiersza
  poleceń).
- [Flasher w przeglądarce](https://psobiech.github.io/opengr8on-clu-esp32/) (Web Serial):
  instalacja, aktualizacja z zachowaniem danych, wgrywanie samych ustawień; sprawdzanie typu
  układu; monitor UART z kolorowaniem logów; obraz ustawień generowany w przeglądarce (do
  pobrania jako `cfg.bin`).
- Partycja `cfg` z ustawieniami (numer seryjny, PIN, WiFi, MQTT, hasło OTA) generowana za pomocą
  `tools/mknvs.py` albo flashera.
- Aktualizacja przez sieć (OTA): HTTP z hasłem, strona `http://IP_CLU:8080/`, `curl`, flasher
  (CORS); weryfikacja obrazu (układ, SHA-256, nazwa projektu).
- Automatyczne cofnięcie nieudanej aktualizacji (brak adresu IP albo awaria w ciągu 60 s).
- Wybór płytki w menuconfig (`CLU_BOARD`) ustawia piny i sprzęt; profil Custom dla własnych płytek.

## Zgodność z Object Managerem

Szczegóły: [Protokół UDP i usługi CLU](../README.md#protokół-udp-i-usługi-clu).

- Urządzenie widziane przez OM jako **CLU_ZWAVE_2** (firmware 5.15.24-2635, fwApiVer 0x203) albo
  **GATE HTTP** (firmware 1.4.2-2346, fwApiVer 0x578), opcja `CLU_DEVICE`.
- Protokół UDP/1234 z AES-128-CBC: discovery (`req_discovery_clu`), `req_set_key`, `req_reset`,
  `req:` (wykonanie Lua), clientRegister / clientReport.
- Ustawienie statycznego IP z OM (`req_set_clu_ip`), osobno dla WiFi i Ethernetu; `0.0.0.0` =
  powrót do DHCP; serwery DNS z cech 25/26.
- Serwer TFTP (`req_start_ftp` / `req_tftp_stop`, RFC 1350, octet i netascii): dyski `a:\` (FAT)
  i `p:\`; wgrywanie projektu (OM.LUA, USER.LUA); jeden transfer naraz, wysyłany plik podmieniany
  dopiero po odebraniu całości (`TFTP.TMP`).
- Tożsamość CLU w NVS: numer seryjny, MAC, PIN, klucze projektu; fabryczny klucz projektu
  wyprowadzany z PIN-u; pliki na `a:\` odczytywane przez OM (`CONFIG.JSON`, `config.txt`).
- Diagnostyka OM: `req_diagnostic_refresh` → `m:\DIAGNOSTIC_PACK.JSON`.
- Logi w OM (Diagnostyka › Logi) przez telnet.
- Wyszukiwanie nowych modułów z OM (`req_refresh_modules`), lista w `a:\CONFIG.JSON`
  (`tfbusDevices`).
- Miękki reset: `req_reset` przeładowuje Lua bez restartu układu (`CLU_SOFT_RESET`).
- Brak rejestracji w chmurze Grentona (zastępczy `a:\CLOUD-PUBLIC.CRT`).
- Współpraca z rozszerzeniem VS Code [opengr8on-vscode](https://github.com/psobiech/opengr8on-vscode)
  zamiast OM.

## Lua i obiekty wirtualne

Szczegóły: [Lua i obiekty](../README.md#lua-i-obiekty).

- Lua 5.3.6 (jak w CLU) albo 5.5.1 (eksperymentalnie); domyślnie liczby `float` i 64-bitowe
  liczby całkowite, opcjonalnie `double` albo tryb 32-bitowy.
- MAIN.LUA / OM.LUA / USER.LUA, model obiektów OM (OBJECT, SYSTEM), `SYSTEM.Init` / `SYSTEM.Loop`.
- Tryb awaryjny: błąd MAIN.LUA albo pętla restartów wywołana skryptem → `BAD_M.LUA` +
  `EMERGNCY.LUA`.
- Lokalna CLU (klasa 0): Uptime, State, Date/Time, AddToLog, SetDateTime, zdarzenie OnTimeChange.
- Zdalna CLU (klasa 1): `execute(0, "lua")`, `get` / `set` / `execute`; w zdarzeniach wywołanie
  czeka na odpowiedź (korutyna).
- Obiekty wirtualne: Timer, Calendar (cron), EventScheduler, PresenceSensor v2, Storage (trwałe
  zmienne w `a:\STORAGE.DAT`), Scheduler, Thermostat v2, SunriseSunsetCalendar v3, Statistics,
  StatisticsPulse, Push (tylko wpis do logu).
- Cechy użytkownika CLU (zmienne z OM.LUA).
- Zdarzenia obiektów (`add_event`), `SYSTEM.Wait`, `SYSTEM:clientRegister` / `clientDestroy` /
  `fetchValues` (raporty wartości do OM), `logDebug` / `logInfo` / `logWarning` / `logError` /
  `print` do logu firmware.
- Limit sterty Lua (`CLU_LUA_MEM_LIMIT_KB`), rezerwa wewnętrznej pamięci RAM dla systemu, limit czasu
  `req:` 2,5 s.
- Opcjonalna prekompilacja wbudowanych bibliotek Lua do kodu bajtowego (bytecode).

## Magistrala TF-Bus

Szczegóły: [TFBUS.md](TFBUS.md#wspólne-zasady), [Projekt](../README.md#projekt).

- Master RS485 half-duplex: RESET / SEEK / ADDR_ASSIGN, DISCOVER, VERSION_QUERY, odpytywanie
  (STATUS_POLL), kolejność ramek zgodna z przechwytem z prawdziwej CLU.
- Rejestr modułów w NVS: stały adres przypisany do numeru seryjnego, zachowywany także po restarcie
  i po chwilowym zniknięciu modułu.
- Magistrala działa od startu, także bez projektu; sterowniki ruszają po `SYSTEM.Init`.
- Powrót modułów z trybu offline: ponowne adresowanie, a po powrocie wysłanie ostatniego stanu ze
  skryptów.
- Rozpoznawanie awarii magistrali (np. restartujący się moduł trzyma linię): moduły nie przechodzą
  od razu w stan offline, a po ustąpieniu awarii następuje szybkie ponowne dołączanie.
- Ochrona przed spóźnionymi odpowiedziami (`CLU_TFBUS_LATE_REPLY_GUARD_MS`).
- Ponowne wysłanie stanu modułu po odrzuceniu komendy z pełnej kolejki
  (`CLU_TFBUS_RESYNC_ON_QUEUE_DROP`).
- Liczniki błędów odbioru UART: przepełnienia, błędy ramki, break (`CLU_TFBUS_UART_ERROR_STATS`).
- Statystyki magistrali: moduły online / łącznie, pominięte odpytania, obciążenie i zajętość linii.
- Konfigurowalne czasy: przerwa między rundami, limity czasu (timeouty) odpytania i ACK, tempo
  ponownego dołączania.

## Bezpieczeństwo

Szczegóły: [Bezpieczeństwo](../README.md#bezpieczeństwo).

- Lua uruchamia tylko tekst (bytecode odrzucany), usunięte `string.dump`, `package.loadlib`,
  większość funkcji `debug`.
- Limity: głębokość rekurencji C w Lua, rozmiar sterty Lua, czas wykonania `req:`; stack
  protector, watchpoint stosu, zatruwanie sterty.
- Okno konfiguracji po starcie lub po wciśnięciu przycisku (`CLU_SETUP_WINDOW_S`): discovery, zmiana
  klucza / IP, TFTP działają tylko w tym oknie.
- TFTP tylko dla hosta z `req_start_ftp`, limit bezczynności 5 min, pliki do 2 MB.
- Losowe PIN-y A–Z0–9 (~41 bitów); PIN wypisywany tylko na UART, nigdy przez telnet.
- MQTT nigdy nie wykonuje Lua.
- OTA tylko z hasłem, porównanie w stałym czasie, opóźnienie ~2 s po błędnym haśle.
- Brak połączeń z chmurą Grentona.

## Sprzęt

Szczegóły: [DEVICES.md](DEVICES.md).

- [Waveshare ESP32-S3-RS485-CAN](devices/waveshare-esp32-s3-rs485-can.md): WiFi, RS485 i RTC na
  płytce.
- [KinCony F8](devices/kincony-f8.md): Ethernet W5500, 8 przekaźników + 8 wejść, wejścia analogowe,
  1-Wire, RTC, OLED.
- [KinCony KC868-A8v3](devices/kincony-kc868-a8v3.md): Ethernet W5500, 8 przekaźników + 8 wejść,
  RTC.
- [Grenton GATE](devices/grenton-gate.md): ESP32-WROVER-B, Ethernet LAN8720A.
- [Inne płytki ESP32-S3](devices/other-esp32-s3.md) z PSRAM i własnym transceiverem RS485.

## Testy

Szczegóły: [Testy](../README.md#testy).

- Testy hosta (Linux, ASan, UBSan) kompilujące źródła firmware z zaślepkami ESP-IDF.
- Sterowniki TF-Bus porównywane bajt po bajcie z przechwytem z prawdziwej CLU.
- Prelude Lua testowane z prawdziwym OM.LUA; Lua 5.3, 5.5, tryb 32-bitowy.
- Testy emulatora (prawdziwy obraz ESP32-S3 w `esp-emu`): start, HTTP, UDP.
- Testy JS flashera (obraz ustawień, OTA).
- Pomiar pokrycia C i Lua.
