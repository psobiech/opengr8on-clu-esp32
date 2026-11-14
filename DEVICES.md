# Obsługiwane urządzenia

Płytki, na których działa opengr8on-clu-esp32: piny, profile `sdkconfig` i uwagi sprzętowe.
Budowanie i opcje menuconfig: [README](README.md).

## Wymagania pamięci

**PSRAM jest wymagany.** Sam runtime Lua, zanim wczyta się projekt OM, zajmuje ok. 170 KB
(biblioteki standardowe, `prelude.lua`, obiekty wirtualne z `objects.lua`, `mqtt.lua`,
`http.lua`), a projekt OM, skrypty i treści HTTP / MQTT dochodzą do tego. ESP32 bez PSRAM (np.
moduły ESP32-WROOM-32) ma po starcie WiFi i usług ok. 65 KB wolnego wewnętrznego RAM, więc Lua się
na nim nie uruchomi (w logu `Lua VM setup: not enough memory`; sieć, OTA i telnet działają dalej).
Obsługiwane płytki mają 8 MB PSRAM. Flash: 4 MB
wystarcza (`partitions_4mb.csv`), na ESP32-S3 profile domyślnie zakładają 16 MB;
`gate-http-N8R8` obsługuje 8 MB flash i 8 MB octal PSRAM (`partitions_8mb.csv`).

- [Waveshare ESP32-S3-RS485-CAN](#waveshare-esp32-s3-rs485-can) — zalecana płytka (z zewnętrzną anteną)
- [Inne płytki ESP32-S3](#inne-płytki-esp32-s3) — własny transceiver RS485
- [KinCony F8 i KC868-A8v3](#kincony-f8-i-kc868-a8v3) — ESP32-S3, W5500, 8 przekaźników / 8 wejść
- [Grenton GATE](#grenton-gate) — ESP32-WROVER-B, Ethernet LAN8720A

## Waveshare ESP32-S3-RS485-CAN

Zalecana płytka: **[Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/wiki/ESP32-S3-RS485-CAN)** (ESP32-S3R8, 16 MB flash, 8 MB PSRAM, RS485 na płytce; [sklep Kamami](https://kamami.pl/moduly-wifi/1201081-industrial-esp32-s3-control-board-with-rs485-and-can-communication-interfaces-supports-wi-fi-blue-5902186330269.html)).
Piny z przykładów producenta (`WS_GPIO.h`), domyślne w menuconfig:

| Sygnał | GPIO |
|---|---|
| RS485 TX | 17 |
| RS485 RX | 18 |
| RS485 DE | 21 (sterowany jako UART RTS w `UART_MODE_RS485_HALF_DUPLEX`) |
| CAN TX / RX | 15 / 16 (nieużywane) |
| RTC PCF85063 SDA / SCL | 39 / 38 (INT 40 nieużywany) |

Podłącz A/B RS485 płytki do A/B TF-Bus oraz GND.

Zegar RTC ustawia czas systemowy przy starcie, zanim ruszą skrypty, więc mają poprawny czas bez
sieci. Każda synchronizacja SNTP i `SetDateTime` zapisuje go z powrotem. Czas przetrwa zanik zasilania
tylko z ogniwem podtrzymującym w złączu baterii RTC (J7); bez niego po włączeniu RTC
zgłasza czas jako nieważny i zegar jest błędny do pierwszej synchronizacji SNTP.

## Inne płytki ESP32-S3

Wymagane: ESP32-S3 z 16 MB flash i 8 MB PSRAM (octal), transceiver RS485 3,3 V, zasilanie z 24 V
magistrali przez przetwornicę.

Przykładowa płytka: [ESP32-S3 N8R8 z Kamami](https://kamami.pl/esp32/1184834-esp32-s3-microcontroller-2-4ghz-wi-fi-development-board-240mhz-dual-core-processor-esp32-s3-wroom-5906623423545.html).
Wariant N8R8 ma 8 MB flash: dla GATE HTTP użyj presetu `gate-http-N8R8` i obrazów wydania
z końcówką `-N8R8` (bez RS485). Pozostałe profile ESP32-S3 nadal wymagają 16 MB flash.

```
 ESP32-S3                3.3 V RS485 transceiver            TFBUS cable
 GPIO17 (TX) ─────────── DI                          A ──── A
 GPIO18 (RX) ─────────── RO                          B ──── B
 GPIO21 (DE) ─────────── DE + /RE (tied)                    GND (isolated side)
 3V3, GND ────────────── VCC, GND                           +24 V → buck → 5 V → board
```

- Użyj transceivera 3,3 V (MAX3485, SP3485, THVD1450) albo izolowanego (ADM2587E, ISO3082).
  Moduł MAX485 na 5 V wystawia RO na poziomie 5 V, czego GPIO S3 nie tolerują.
- Moduły z automatycznym kierunkiem (bez pinu DE) działają z `CLU_RS485_DE_GPIO=-1`. Wolniej
  przełączają kierunek, więc szybko odpowiadające moduły mogą zostać pominięte.
- Zamontuj terminator 120 Ω po stronie CLU oraz rezystory polaryzujące, jeśli transceiver nie ma
  funkcji fail-safe.
- **Nigdy nie podłączaj dwóch masterów (prawdziwej CLU i tej) do tej samej magistrali.**

## KinCony F8 i KC868-A8v3

Obie płytki: ESP32-S3-WROOM-1U N16R8 (16 MB flash, 8 MB PSRAM), Ethernet W5500 na SPI, RS485 i
8 przekaźników + 8 wejść na jednym ekspanderze PCF8575. Profile `sdkconfig.defaults.f8` i
`sdkconfig.defaults.a8v3`, presety `f8` / `a8v3` (zob. [Budowanie](README.md#budowanie)):

- [KinCony F8 — strona produktu](https://www.kincony.com/kincony-f8-esp32-s3-16a-removable-relay-board.html)
- [KinCony KC868-A8v3 — strona produktu](https://www.kincony.com/esp32-s3-8-channel-relay-module.html)

| Funkcja | KinCony F8 | KC868-A8v3 | Opcja |
|---|---|---|---|
| W5500: SCLK / MOSI / MISO / CS | 42 / 43 / 44 / 41 | 1 / 2 / 41 / 42 | `CLU_ETH_SPI_*` |
| W5500: INT / RST | 2 / 1 | 43 / 44 | `CLU_ETH_SPI_INT_GPIO`, `CLU_ETH_SPI_RST_GPIO` |
| RS485 TX / RX (bez DE, automatyczny kierunek) | 16 / 17 | 39 / 38 | `CLU_RS485_*` |
| I2C SDA / SCL | 8 / 18 | 8 / 18 | `CLU_LOCAL_IO_SDA_GPIO`, `CLU_LOCAL_IO_SCL_GPIO` |
| PCF8575: adres | 0x24 | 0x22 | `CLU_LOCAL_IO_ADDR` |
| PCF8575: przekaźniki / wejścia | P10–P17 / P00–P07 | P00–P07 / P10–P17 | `CLU_LOCAL_IO_OUT_PINS` |
| Przycisk konfiguracji | 0 (BOOT) | 0 (BOOT) | `CLU_SETUP_BUTTON_GPIO` |

- **Przekaźniki i wejścia płytki** widać w OM jako jeden moduł **IO_MODULE_DIN_8** (8 DOUT + 8 DIN;
  kanał 1 = przekaźnik 1 / DI1). Firmware sam odpowiada na ramki TF-Bus tego modułu, więc bierze
  on udział w wykrywaniu, adresowaniu i odpytywaniu jak moduły na RS485 i trafia do
  `tfbusDevices` w `a:\CONFIG.JSON`. Dodaj go do projektu w OM jak każdy wykryty moduł.
- Numer seryjny modułu jest losowany przy pierwszym starcie i zapisywany w NVS (przestrzeń
  `localio`), jak numer CLU; `CLU_LOCAL_IO_SN` w menuconfig go nadpisuje.
- Wyjścia i wejścia są aktywne stanem niskim (`CLU_LOCAL_IO_OUT_ACTIVE_LOW` /
  `IN_ACTIVE_LOW`), jak w konfiguracjach ESPHome producenta; niesprawdzone na sprzęcie.
- Wejścia są próbkowane co 5 ms przy odpytywaniu modułu; zmiana liczy się po dwóch zgodnych
  próbkach. Ekspander, który nie odpowiada, oznacza moduł jako offline.
- Przekaźniki są wyłączone od włączenia zasilania (PCF8575 startuje ze stanem wysokim). Po
  wykryciu modułu (start, przeładowanie projektu) CLU wysyła mu stan początkowy tak jak prawdziwym
  modułom: wszystkie wyłączone, potem to, co ustawią skrypty.
- W5500 zajmuje GPIO43/44 (piny konsoli UART0), więc **konsola i log idą przez natywne USB
  (złącze USB-C, USB-Serial-JTAG)**; tamtędy też wgrywa się firmware.
- Zegar RTC DS3231 (0x68) jest na tej samej magistrali I2C co PCF8575; profile go włączają
  (`CLU_RTC_CHIP` = DS3231, SDA 8 / SCL 18). Czas przetrwa zanik zasilania tylko z baterią
  podtrzymującą.
- WiFi domyślnie wyłączone, można włączyć obok Ethernetu.
- Nieużywane: wejścia analogowe, 1-wire, karta SD, EEPROM, wyświetlacz, RF 433 MHz, moduł Tuya.
- RS485 bez pinu DE: zob. uwagę o transceiverach z automatycznym kierunkiem w
  [Inne płytki ESP32-S3](#inne-płytki-esp32-s3).

## Grenton GATE

Preset `gate`, target `esp32` (`sdkconfig.defaults.esp32`, `partitions_4mb.csv`): ESP32-WROVER-B (4 MB flash,
PSRAM), Ethernet przez wewnętrzny EMAC + PHY LAN8720A. Domyślne piny:

| Funkcja | GPIO | Opcja |
|---|---|---|
| LAN8720A: MDC / MDIO, adres PHY | 23 / 18, 0 | `CLU_ETH_MDC_GPIO`, `CLU_ETH_MDIO_GPIO`, `CLU_ETH_PHY_ADDR` |
| LAN8720A: reset | 2 | `CLU_ETH_PHY_RST_GPIO` |
| RMII 50 MHz | wejście z oscylatora na GPIO0 | `CLU_ETH_CLK` |
| RMII dane (stałe w ESP32) | TXEN 21, TXD0 19, TXD1 22, CRS_DV 27, RXD0 25, RXD1 26 | — |
| RS485 (UART1) TX / RX / DE | 4 / 3 / 5 | `CLU_RS485_*` |
| Przycisk reset (aktywny niski) | 34 | `CLU_SETUP_BUTTON_GPIO` |

- WiFi i Ethernet mogą działać równolegle (`CLU_WIFI` + `CLU_ETH`); bez Ethernetu WiFi jest
  zawsze włączone. Domyślnie na `esp32`: tylko Ethernet.
- RTC (`CLU_RTC`) domyślnie wyłączony na `esp32` (GPIO38/39 to na ESP32 tylko wejścia).
- RX RS485 na GPIO3 to pin U0RXD: konsola UART nie przyjmuje wejścia (log wychodzi dalej na TX).
- Partycje: 2 × 1,5 MB OTA, 896 KB FAT.

### Wgrywanie na GATE z włączonym szyfrowaniem flash

Wyłącz szyfrowanie, dopalając
kolejny bit `FLASH_CRYPT_CNT` (parzysta liczba ustawionych bitów = szyfrowanie wyłączone), potem
wgraj całość od zera:

```bash
espefuse.py -p /dev/ttyUSB0 summary                    # FLASH_CRYPT_CNT: nieparzysta = włączone
espefuse.py -p /dev/ttyUSB0 burn_efuse FLASH_CRYPT_CNT # +1 bit -> wyłączone
idf.py --preset gate -p /dev/ttyUSB0 erase-flash flash monitor
```

> **UWAGA: eFuse są nieodwracalne.** `FLASH_CRYPT_CNT` ma 7 bitów, więc szyfrowanie można przełączać
> tylko kilka razy. Po dopaleniu eFuse fabryczny firmware przestaje się uruchamiać i nie jest możliwe jego przywrócenie!
