# KinCony KC868-A8v3

ESP32-S3-WROOM-1U N16R8 (16 MB flash, 8 MB PSRAM), Ethernet W5500 na SPI, RS485 i
8 przekaźników + 8 wejść na jednym ekspanderze PCF8575. Płytka `KinCony KC868-A8v3` w menuconfig
(**Board**), preset `a8v3` (zob. [Budowanie](../../README.md#budowanie)). Pozostałe płytki:
[Obsługiwane urządzenia](../DEVICES.md).

- [KinCony KC868-A8v3 — strona produktu](https://www.kincony.com/esp32-s3-8-channel-relay-module.html)
- [Konfiguracja ESPHome](https://devices.esphome.io/devices/kincony-kc868-a8v3/) (piny zgodne z opcją Board)

| Funkcja | GPIO / wartość | Opcja |
|---|---|---|
| W5500: SCLK / MOSI / MISO / CS | 1 / 2 / 41 / 42 | `CLU_ETH_SPI_*` |
| W5500: INT / RST | 43 / 44 | `CLU_ETH_SPI_INT_GPIO`, `CLU_ETH_SPI_RST_GPIO` |
| RS485 TX / RX (bez DE, automatyczny kierunek) | 39 / 38 | `CLU_RS485_*` |
| I2C SDA / SCL | 8 / 18 | `CLU_LOCAL_IO_SDA_GPIO`, `CLU_LOCAL_IO_SCL_GPIO` |
| PCF8575: adres | 0x22 | `CLU_LOCAL_IO_ADDR` |
| PCF8575: przekaźniki / wejścia | P00–P07 / P10–P17 | `CLU_LOCAL_IO_OUT_PINS` |
| Przycisk konfiguracji | 0 (BOOT) | `CLU_SETUP_BUTTON_GPIO` |

Na magistrali I2C są: PCF8575 (0x22), EEPROM (0x50, nieużywany) i RTC DS3231 (0x68).

- **Przekaźniki i wejścia płytki** widać w OM jako jeden moduł **IO_MODULE_DIN_8** (8 DOUT + 8 DIN;
  kanał 1 = przekaźnik 1 / DI1). Firmware sam odpowiada na ramki TF-Bus tego modułu, więc bierze
  on udział w wykrywaniu, adresowaniu i odpytywaniu jak moduły na RS485 i trafia do
  `tfbusDevices` w `a:\CONFIG.JSON`. Dodaj go do projektu w OM jak każdy wykryty moduł.
- **Zaciski wejść są w odwrotnej kolejności: skrajny lewy to DI8**, skrajny prawy DI1.
- Wejście jest aktywne po zwarciu DI z GND tej samej listwy zacisków (styk bezpotencjałowy);
  otwarte wejście ma ok. 23 V przy zasilaniu 24 V. Dioda wejścia świeci, gdy jest aktywne.
- Wyjścia i wejścia są aktywne stanem niskim (`CLU_LOCAL_IO_OUT_ACTIVE_LOW` /
  `IN_ACTIVE_LOW`); sprawdzone na sprzęcie.
- Numer seryjny modułu jest losowany przy pierwszym starcie i zapisywany w NVS (przestrzeń
  `localio`), jak numer CLU; `CLU_LOCAL_IO_SN` w menuconfig go nadpisuje.
- Wejścia są próbkowane co 5 ms przy odpytywaniu modułu; zmiana liczy się po dwóch zgodnych
  próbkach. Ekspander, który nie odpowiada, oznacza moduł jako offline.
- Przekaźniki są wyłączone od włączenia zasilania (PCF8575 startuje ze stanem wysokim). Po
  wykryciu modułu (start, przeładowanie projektu) CLU wysyła mu stan początkowy tak jak prawdziwym
  modułom: wszystkie wyłączone, potem to, co ustawią skrypty.
- **Ręczne przełączniki przekaźników** (przełącznik ściągający pin przekaźnika PCF8575 do masy, jak
  w KinCony F8) są widoczne w OM: przekaźnik włączony przełącznikiem ma `Value` = 1 w swoim DOUT i
  wywołuje OnSwitchOn (wyłączenie przełącznika: OnSwitchOff). Firmware odczytuje stan pinów
  przekaźników PCF8575 (próbkowanie i filtrowanie jak przy wejściach); bez przełącznika odczyt
  zgadza się z tym, co ustawił CLU. Przełącznik widać tylko, gdy CLU trzyma przekaźnik wyłączony;
  przekaźnik włączony z CLU jest zawsze widziany jako włączony. Wyłączenie z CLU przekaźnika
  trzymanego przełącznikiem nic nie zmienia (przekaźnik zostaje włączony), ale `Value` od razu ma
  0 aż do zmiany stanu przełącznika.
- W5500 zajmuje GPIO43/44 (piny konsoli UART0), więc **konsola i log idą przez natywne USB
  (złącze USB-C, USB-Serial-JTAG)**; tamtędy też wgrywa się firmware.
- Zegar RTC DS3231 (0x68) jest na tej samej magistrali I2C co PCF8575; profil go włącza
  (`CLU_RTC_CHIP` = DS3231, SDA 8 / SCL 18). Czas przetrwa zanik zasilania tylko z baterią
  podtrzymującą.
- WiFi domyślnie wyłączone, można włączyć obok Ethernetu.
- Nieużywane: wejścia analogowe, 1-wire, EEPROM, wyświetlacz, moduł Tuya.
- RS485 bez pinu DE: zob. uwagę o transceiverach z automatycznym kierunkiem w
  [Inne płytki ESP32-S3](other-esp32-s3.md#transceiver-rs485).

---

# KinCony KC868-A8v3

ESP32-S3-WROOM-1U N16R8 (16 MB flash, 8 MB PSRAM), W5500 Ethernet over SPI, RS485, and
8 relays + 8 inputs on a single PCF8575 expander. Board `KinCony KC868-A8v3` in menuconfig
(**Board**), preset `a8v3` (see [Building](../../README.md)). Other boards:
[Supported devices](../DEVICES.md#supported-devices).

- [KinCony KC868-A8v3 — product page](https://www.kincony.com/esp32-s3-8-channel-relay-module.html)
- [ESPHome configuration](https://devices.esphome.io/devices/kincony-kc868-a8v3/) (pins match the Board option)

| Function | GPIO / value | Option |
|---|---|---|
| W5500: SCLK / MOSI / MISO / CS | 1 / 2 / 41 / 42 | `CLU_ETH_SPI_*` |
| W5500: INT / RST | 43 / 44 | `CLU_ETH_SPI_INT_GPIO`, `CLU_ETH_SPI_RST_GPIO` |
| RS485 TX / RX (no DE, auto-direction) | 39 / 38 | `CLU_RS485_*` |
| I2C SDA / SCL | 8 / 18 | `CLU_LOCAL_IO_SDA_GPIO`, `CLU_LOCAL_IO_SCL_GPIO` |
| PCF8575: address | 0x22 | `CLU_LOCAL_IO_ADDR` |
| PCF8575: relays / inputs | P00–P07 / P10–P17 | `CLU_LOCAL_IO_OUT_PINS` |
| Setup button | 0 (BOOT) | `CLU_SETUP_BUTTON_GPIO` |

On the I2C bus: PCF8575 (0x22), EEPROM (0x50, unused), and DS3231 RTC (0x68).

- **Board relays and inputs** appear in OM as a single **IO_MODULE_DIN_8** module (8 DOUT + 8 DIN;
  channel 1 = relay 1 / DI1). The firmware answers TF-Bus frames for this module itself, so it
  participates in discovery, addressing, and polling just like RS485 modules and is included in
  `tfbusDevices` in `a:\CONFIG.JSON`. Add it to your OM project like any discovered module.
- **Input terminals are in reverse order: leftmost is DI8**, rightmost is DI1.
- An input is active when DI is shorted to GND on the same terminal strip (dry contact);
  an open input reads approx. 23 V with a 24 V supply. The input LED lights up when active.
- Outputs and inputs are active low (`CLU_LOCAL_IO_OUT_ACTIVE_LOW` /
  `IN_ACTIVE_LOW`); verified on hardware.
- The module serial number is generated randomly on first boot and saved in NVS (`localio`
  namespace), like the CLU serial number; `CLU_LOCAL_IO_SN` in menuconfig overrides it.
- Inputs are sampled every 5 ms during module polling; a change counts after two matching
  samples. An unresponsive expander marks the module offline.
- Relays are off from power-on (PCF8575 boots with pins high). After
  module discovery (boot, project reload), the CLU sends it the initial state just like to real
  modules: all off, then whatever scripts set.
- **Manual relay switches** (a switch pulling the PCF8575 relay pin to GND, as on the KinCony F8)
  are visible in OM: a relay switched on by hand has `Value` = 1 in its DOUT and fires OnSwitchOn
  (switching it back off: OnSwitchOff). The firmware reads back the PCF8575 relay pins (sampled
  and debounced like the inputs); without a switch the readback matches what the CLU set. A switch
  is only seen while the CLU holds that relay off; a relay the CLU switched on always reads as on.
  Switching off from the CLU a relay held on by its switch changes nothing (the relay stays on),
  but `Value` reads 0 right away until the switch changes state.
- W5500 uses GPIO43/44 (UART0 console pins), so **console and logs go over native USB
  (USB-C port, USB-Serial-JTAG)**; firmware flashing also uses it.
- DS3231 RTC (0x68) is on the same I2C bus as PCF8575; the profile enables it
  (`CLU_RTC_CHIP` = DS3231, SDA 8 / SCL 18). Time survives power loss only with a backup battery
  installed.
- WiFi is disabled by default and can be enabled alongside Ethernet.
- Unused: analog inputs, 1-wire, EEPROM, display, Tuya module.
- RS485 without DE pin: see the note on auto-direction transceivers in
  [Other ESP32-S3 boards](other-esp32-s3.md#rs485-transceiver).
