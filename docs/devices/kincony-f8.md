# KinCony F8

ESP32-S3-WROOM-1U N16R8 (16 MB flash, 8 MB PSRAM), Ethernet W5500 na SPI, RS485 i
8 przekaźników + 8 wejść na jednym ekspanderze PCF8575, 5 dodatkowych wejść na GPIO. Płytka `KinCony F8` w menuconfig (**Board**), preset `f8`
(zob. [Budowanie](../../README.md#budowanie)). Pozostałe płytki: [Obsługiwane urządzenia](../DEVICES.md).

- [KinCony F8 — strona produktu](https://www.kincony.com/kincony-f8-esp32-s3-16a-removable-relay-board.html)
- [KinCony F8 — ESPHome Devices](https://devices.esphome.io/devices/kincony-f8/)

Przetestowane na sprzęcie: przekaźniki i wejścia na PCF8575, wejścia na GPIO (zaciski 1-wire i
wolny GPIO) oraz wyświetlacz działają. Wejścia analogowe nie są obsługiwane.

**Zasilanie i przekaźniki:** fabrycznie montowane są przekaźniki na 12 V, ale są wymienne (w
podstawkach) i można założyć wersje na 24 V; płytka pracuje też przy zasilaniu 24 V. Napięcie
cewek przekaźników musi odpowiadać napięciu zasilania płytki.

| Funkcja | GPIO / wartość | Opcja |
|---|---|---|
| W5500: SCLK / MOSI / MISO / CS | 42 / 43 / 44 / 41 | `CLU_ETH_SPI_*` |
| W5500: INT / RST | 2 / 1 | `CLU_ETH_SPI_INT_GPIO`, `CLU_ETH_SPI_RST_GPIO` |
| RS485 TX / RX (bez DE, automatyczny kierunek) | 16 / 17 | `CLU_RS485_*` |
| I2C SDA / SCL | 8 / 18 | `CLU_LOCAL_IO_SDA_GPIO`, `CLU_LOCAL_IO_SCL_GPIO` |
| PCF8575: adres | 0x24 | `CLU_LOCAL_IO_ADDR` |
| PCF8575: przekaźniki / wejścia | P10–P17 / P00–P07 | `CLU_LOCAL_IO_OUT_PINS` |
| Wejścia GPIO (DIN1–DIN5 drugiego modułu) | 48 / 47 / 21 / 15 / 14 | `CLU_LOCAL_IO_GPIO_INPUTS` |
| Wyświetlacz SSD1306 128x64: adres | 0x3C | `CLU_DISPLAY_ADDR` |
| Przycisk konfiguracji | 0 (BOOT) | `CLU_SETUP_BUTTON_GPIO` |

- **Przekaźniki i wejścia płytki** widać w OM jako jeden moduł **IO_MODULE_DIN_8** (8 DOUT + 8 DIN;
  kanał 1 = przekaźnik 1 / DI1). Firmware sam odpowiada na ramki TF-Bus tego modułu, więc bierze
  on udział w wykrywaniu, adresowaniu i odpytywaniu jak moduły na RS485 i trafia do
  `tfbusDevices` w `a:\CONFIG.JSON`. Dodaj go do projektu w OM jak każdy wykryty moduł.
- **Wejścia na GPIO** (4 zaciski 1-wire GPIO48, 47, 21, 15 z podciąganiem na płytce i wolny zacisk
  GPIO14 bez podciągania) to drugi moduł **IO_MODULE_DIN_8**: DIN1–DIN5 w tej kolejności,
  DIN6–DIN8 zawsze nieaktywne. Jego wyjścia DOUT niczego nie sterują. Firmware włącza wewnętrzne
  podciąganie; wejście jest aktywne po zwarciu do GND. Zaciski 1-wire nie mogą wtedy służyć do
  czujników 1-wire. Pusta lista w `CLU_LOCAL_IO_GPIO_INPUTS` (płytka `Custom`) wyłącza ten moduł.
  Gdy PCF8575 nie odpowiada, oba moduły są offline (wspólne odpytywanie).
- Numery seryjne modułów są losowane przy pierwszym starcie i zapisywane w NVS (przestrzeń
  `localio`), jak numer CLU; `CLU_LOCAL_IO_SN` / `CLU_LOCAL_IO_SN2` w menuconfig je nadpisują.
- Wyjścia i wejścia są aktywne stanem niskim (`CLU_LOCAL_IO_OUT_ACTIVE_LOW` /
  `IN_ACTIVE_LOW`), jak w konfiguracjach ESPHome producenta; sprawdzone na sprzęcie.
- Wejścia są próbkowane co 5 ms przy odpytywaniu modułu; zmiana liczy się po dwóch zgodnych
  próbkach. Ekspander, który nie odpowiada, oznacza moduł jako offline.
- Przekaźniki są wyłączone od włączenia zasilania (PCF8575 startuje ze stanem wysokim). Po
  wykryciu modułu (start, przeładowanie projektu) CLU wysyła mu stan początkowy tak jak prawdziwym
  modułom: wszystkie wyłączone, potem to, co ustawią skrypty.
- W5500 zajmuje GPIO43/44 (piny konsoli UART0), więc **konsola i log idą przez natywne USB
  (złącze USB-C, USB-Serial-JTAG)**; tamtędy też wgrywa się firmware.
- Zegar RTC DS3231 (0x68) jest na tej samej magistrali I2C co PCF8575; profil go włącza
  (`CLU_RTC_CHIP` = DS3231, SDA 8 / SCL 18). Czas przetrwa zanik zasilania tylko z baterią
  podtrzymującą.
- WiFi domyślnie wyłączone, można włączyć obok Ethernetu.
- Nieużywane: wejścia analogowe, czujniki 1-wire, karta SD, EEPROM, RF 433 MHz, moduł Tuya.
- RS485 bez pinu DE: zob. uwagę o transceiverach z automatycznym kierunkiem w
  [Inne płytki ESP32-S3](other-esp32-s3.md#transceiver-rs485).

## Wyświetlacz

OLED SSD1306 128x64 (0x3C) na tej samej magistrali I2C co PCF8575 i DS3231. Profil go włącza
(`CLU_DISPLAY`). Firmware pokazuje na nim ekran statusu, odświeżany co sekundę:

```
CLU 123456789
ETH 192.168.1.50
WiFi -

2026-11-24 12:34:56
TF-Bus 3/3 online
Up 2d 04:05:06
```

- Wiersz `WiFi` jest tylko przy włączonym WiFi; `-` = interfejs bez adresu.
- Czas lokalny (strefa z OM); `Time not set` do pierwszej synchronizacji (RTC, SNTP, OM).
- `TF-Bus` liczy moduły z projektu / ostatniego wykrywania, razem z wbudowanym IO.
- Po I2C idą tylko zmienione wiersze (zwykle zegar i czas pracy), każdy osobnym krótkim
  transferem, więc odpytywanie PCF8575 co 5 ms nie czeka dłużej niż kilka ms.
- Brak wyświetlacza jest logowany i pomijany.
- Ekran nie jest dostępny ze skryptów Lua ani z OM.

---

# KinCony F8

ESP32-S3-WROOM-1U N16R8 (16 MB flash, 8 MB PSRAM), W5500 Ethernet over SPI, RS485, and
8 relays + 8 inputs on a single PCF8575 expander, plus 5 additional GPIO inputs. Board `KinCony F8` in menuconfig (**Board**), preset `f8`
(see [Building](../../README.md)). Other boards: [Supported devices](../DEVICES.md#supported-devices).

- [KinCony F8 — product page](https://www.kincony.com/kincony-f8-esp32-s3-16a-removable-relay-board.html)
- [KinCony F8 — ESPHome Devices](https://devices.esphome.io/devices/kincony-f8/)

Tested on hardware: relays and inputs on PCF8575, GPIO inputs (1-wire terminals and
spare GPIO), and the display all work. Analog inputs are not supported.

**Power and relays:** 12 V relays are fitted from the factory, but they are socketed (replaceable)
and 24 V versions can be installed; the board also runs on 24 V power. Relay coil voltage
must match the board supply voltage.

| Function | GPIO / value | Option |
|---|---|---|
| W5500: SCLK / MOSI / MISO / CS | 42 / 43 / 44 / 41 | `CLU_ETH_SPI_*` |
| W5500: INT / RST | 2 / 1 | `CLU_ETH_SPI_INT_GPIO`, `CLU_ETH_SPI_RST_GPIO` |
| RS485 TX / RX (no DE, auto-direction) | 16 / 17 | `CLU_RS485_*` |
| I2C SDA / SCL | 8 / 18 | `CLU_LOCAL_IO_SDA_GPIO`, `CLU_LOCAL_IO_SCL_GPIO` |
| PCF8575: address | 0x24 | `CLU_LOCAL_IO_ADDR` |
| PCF8575: relays / inputs | P10–P17 / P00–P07 | `CLU_LOCAL_IO_OUT_PINS` |
| GPIO inputs (second module DIN1–DIN5) | 48 / 47 / 21 / 15 / 14 | `CLU_LOCAL_IO_GPIO_INPUTS` |
| SSD1306 128x64 display: address | 0x3C | `CLU_DISPLAY_ADDR` |
| Setup button | 0 (BOOT) | `CLU_SETUP_BUTTON_GPIO` |

- **Board relays and inputs** appear in OM as a single **IO_MODULE_DIN_8** module (8 DOUT + 8 DIN;
  channel 1 = relay 1 / DI1). The firmware answers TF-Bus frames for this module itself, so it
  participates in discovery, addressing, and polling just like RS485 modules and is included in
  `tfbusDevices` in `a:\CONFIG.JSON`. Add it to your OM project like any discovered module.
- **GPIO inputs** (4 1-wire terminals GPIO48, 47, 21, 15 with onboard pull-ups and spare terminal
  GPIO14 without pull-up) form a second **IO_MODULE_DIN_8** module: DIN1–DIN5 in that order,
  DIN6–DIN8 always inactive. Its DOUT outputs control nothing. The firmware enables internal
  pull-ups; an input is active when shorted to GND. The 1-wire terminals cannot then be used for
  1-wire sensors. An empty list in `CLU_LOCAL_IO_GPIO_INPUTS` (`Custom` board) disables this module.
  When the PCF8575 does not respond, both modules go offline (shared polling).
- Module serial numbers are generated randomly on first boot and saved in NVS (`localio`
  namespace), like the CLU serial number; `CLU_LOCAL_IO_SN` / `CLU_LOCAL_IO_SN2` in menuconfig
  override them.
- Outputs and inputs are active low (`CLU_LOCAL_IO_OUT_ACTIVE_LOW` /
  `IN_ACTIVE_LOW`), matching the vendor's ESPHome configs; verified on hardware.
- Inputs are sampled every 5 ms during module polling; a change counts after two matching
  samples. An unresponsive expander marks the module offline.
- Relays are off from power-on (PCF8575 boots with pins high). After
  module discovery (boot, project reload), the CLU sends it the initial state just like to real
  modules: all off, then whatever scripts set.
- W5500 uses GPIO43/44 (UART0 console pins), so **console and logs go over native USB
  (USB-C port, USB-Serial-JTAG)**; firmware flashing also uses it.
- DS3231 RTC (0x68) is on the same I2C bus as PCF8575; the profile enables it
  (`CLU_RTC_CHIP` = DS3231, SDA 8 / SCL 18). Time survives power loss only with a backup battery
  installed.
- WiFi is disabled by default and can be enabled alongside Ethernet.
- Unused: analog inputs, 1-wire sensors, SD card, EEPROM, 433 MHz RF, Tuya module.
- RS485 without DE pin: see the note on auto-direction transceivers in
  [Other ESP32-S3 boards](other-esp32-s3.md#rs485-transceiver).

## Display

SSD1306 128x64 OLED (0x3C) on the same I2C bus as PCF8575 and DS3231. The profile enables it
(`CLU_DISPLAY`). The firmware shows a status screen on it, refreshed every second:

```
CLU 123456789
ETH 192.168.1.50
WiFi -

2026-11-24 12:34:56
TF-Bus 3/3 online
Up 2d 04:05:06
```

- The `WiFi` line is only shown when WiFi is enabled; `-` = interface without an address.
- Local time (timezone from OM); `Time not set` until first sync (RTC, SNTP, OM).
- `TF-Bus` counts modules from the project / last discovery, including built-in IO.
- Only changed lines are sent over I2C (usually clock and uptime), each in a separate short
  transfer, so 5 ms PCF8575 polling never waits more than a few ms.
- A missing display is logged and skipped.
- The screen is not accessible from Lua scripts or OM.
