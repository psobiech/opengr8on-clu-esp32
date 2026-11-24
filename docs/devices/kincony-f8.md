# KinCony F8

ESP32-S3-WROOM-1U N16R8 (16 MB flash, 8 MB PSRAM), Ethernet W5500 na SPI, RS485 i
8 przekaźników + 8 wejść na jednym ekspanderze PCF8575, 5 dodatkowych wejść na GPIO, 4 wejścia analogowe i
czujniki temperatury 1-Wire. Płytka `KinCony F8` w menuconfig (**Board**), preset `f8`
(zob. [Budowanie](../../README.md#budowanie)). Pozostałe płytki: [Obsługiwane urządzenia](../DEVICES.md).

- [KinCony F8 — strona produktu](https://www.kincony.com/kincony-f8-esp32-s3-16a-removable-relay-board.html)
- [KinCony F8 — ESPHome Devices](https://devices.esphome.io/devices/kincony-f8/)

Przetestowane na sprzęcie: przekaźniki i wejścia na PCF8575, wejścia na GPIO (zaciski 1-wire i
wolny GPIO) oraz wyświetlacz działają. Wejścia analogowe i czujniki 1-Wire
([niżej](#wejścia-analogowe-i-czujniki-1-wire)) nie są jeszcze sprawdzone na sprzęcie.

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
| Wejścia analogowe A1 / A2 (0–5 V), A3 / A4 (4–20 mA) | 5 / 7 / 6 / 4 | `CLU_LOCAL_IO_ANALOG_INPUTS` |
| Zaciski 1-wire 48 / 47 / 21 / 15: DIN1–DIN4 albo magistrala 1-Wire | DIN | `CLU_LOCAL_IO_F8_DIN1_MODE` … `DIN4_MODE` |
| Wyświetlacz SSD1306 128x64: adres | 0x3C | `CLU_DISPLAY_ADDR` |
| Przycisk konfiguracji | 0 (BOOT) | `CLU_SETUP_BUTTON_GPIO` |

- **Przekaźniki i wejścia płytki** widać w OM jako jeden moduł **IO_MODULE_DIN_8** (8 DOUT + 8 DIN;
  kanał 1 = przekaźnik 1 / DI1). Firmware sam odpowiada na ramki TF-Bus tego modułu, więc bierze
  on udział w wykrywaniu, adresowaniu i odpytywaniu jak moduły na RS485 i trafia do
  `tfbusDevices` w `a:\CONFIG.JSON`. Dodaj go do projektu w OM jak każdy wykryty moduł.
- **Wejścia na GPIO** (4 zaciski 1-wire GPIO48, 47, 21, 15 z podciąganiem na płytce i wolny zacisk
  GPIO14 bez podciągania) to drugi moduł **IO_MODULE_DIN_8**: DIN1–DIN5 w tej kolejności,
  DIN6–DIN8 zawsze nieaktywne. Jego wyjścia DOUT niczego nie sterują. Firmware włącza wewnętrzne
  podciąganie; wejście jest aktywne po zwarciu do GND. Zacisk wybrany na magistralę 1-Wire
  (`CLU_LOCAL_IO_F8_DINx_MODE`) zostaje na swoim kanale, ale jest zawsze nieaktywny. Pusta lista w `CLU_LOCAL_IO_GPIO_INPUTS` (płytka `Custom`) wyłącza ten moduł.
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
- **Ręczne przełączniki przekaźników** są widoczne w OM: przekaźnik włączony przełącznikiem ma
  `Value` = 1 w swoim DOUT i wywołuje OnSwitchOn (wyłączenie przełącznika: OnSwitchOff). Firmware
  odczytuje stan pinów przekaźników PCF8575 (próbkowanie i filtrowanie jak przy wejściach).
  Przełącznik widać tylko, gdy CLU trzyma przekaźnik wyłączony; przekaźnik włączony z CLU jest
  zawsze widziany jako włączony. Wyłączenie z CLU przekaźnika trzymanego przełącznikiem nic nie
  zmienia (przekaźnik zostaje włączony), ale `Value` od razu ma 0 aż do zmiany stanu przełącznika.
- W5500 zajmuje GPIO43/44 (piny konsoli UART0), więc **konsola i log idą przez natywne USB
  (złącze USB-C, USB-Serial-JTAG)**; tamtędy też wgrywa się firmware.
- Zegar RTC DS3231 (0x68) jest na tej samej magistrali I2C co PCF8575; profil go włącza
  (`CLU_RTC_CHIP` = DS3231, SDA 8 / SCL 18). Czas przetrwa zanik zasilania tylko z baterią
  podtrzymującą.
- WiFi domyślnie wyłączone, można włączyć obok Ethernetu.
- Nieużywane: karta SD, EEPROM, RF 433 MHz, moduł Tuya.
- RS485 bez pinu DE: zob. uwagę o transceiverach z automatycznym kierunkiem w
  [Inne płytki ESP32-S3](other-esp32-s3.md#transceiver-rs485).

## Wejścia analogowe i czujniki 1-Wire

Widać je w OM jako moduł **ANALOG_DIN** (jak Grenton ANALOG IN/OUT 2.0) i po jednym module
**ONE_WIRE** na każdy znaleziony czujnik. Firmware sam odpowiada na ich ramki TF-Bus, więc trafiają
do `tfbusDevices` i dodaje się je do projektu jak każdy wykryty moduł. Moduł ANALOG_DIN jest, gdy
ustawione są wejścia analogowe albo magistrala 1-Wire; jego numer seryjny jest w NVS (`sn3`,
nadpisuje go `CLU_LOCAL_IO_SN3`).

- **AnalogIN1–AnalogIN4** = A1–A4. `RawValue` to wartość na zacisku: A1/A2 w woltach (0–5 V,
  napięcie pinu × 1,51), A3/A4 w miliamperach (4–20 mA, napięcie na 150 Ω × 6,667). Współczynniki
  są w `CLU_LOCAL_IO_ANALOG_INPUTS` (`GPIO:współczynnik`). `Value` = `RawValue` × `Scale`;
  `Value%` liczony od `MinValue` do `MaxValue`. `Sensitivity` (w jednostkach `RawValue`) to zmiana,
  przy której idą OnValueChange / OnValueLower / OnValueRise. Wyjście poza zakres `MinValue`–`MaxValue`
  daje OnOutOfRange, powrót OnInRange; dojście do `MaxValue` / `MinValue` daje OnSwitchOn /
  OnSwitchOff. Wejścia są próbkowane co 100 ms (średnia z 16 pomiarów ADC1, tłumienie 12 dB,
  kalibracja fabryczna), więc odczyt jest przybliżony; A1/A2 nasycają się przy ok. 4,7 V.
- **AnalogIN5–AnalogIN7** zawsze 0, **AnalogOUT1–4** niczego nie sterują (płytka nie ma wyjść
  analogowych): cechy pamiętają ustawione wartości, metody nic nie robią.
- **1-Wire**: w menuconfig dla każdego zacisku 1-wire wybierz **Digital input DINx** albo **1-Wire
  bus** (`CLU_LOCAL_IO_F8_DIN1_MODE` … `DIN4_MODE`; GPIO48 / 47 / 21 / 15 = DIN1–DIN4 modułu wejść
  GPIO, podciąganie na płytce). Może to być kilka zacisków (osobne magistrale, np. do różnych
  pomieszczeń); kanał DIN zacisku z magistralą jest zawsze nieaktywny. Wszystkie magistrale są za
  jednym ONE_WIRE_HUB, limit 16 czujników łącznie. Obsługiwane czujniki: DS18B20 i DS18S20, zasilane osobno (bez
  zasilania pasożytniczego), do 16. Każdy czujnik to moduł ONE_WIRE (hwType 0xff, fwType = kod rodziny
  0x28 / 0x10) z obiektem ONEW_SENSOR; numer seryjny modułu to dolne 31 bitów numeru seryjnego
  czujnika z ROM, więc czujnik zachowuje tożsamość po restarcie i po przełożeniu na inną płytkę.
- Magistrala jest przeszukiwana przy starcie, przy „Clear CLU” w OM (czujniki, których już nie ma,
  znikają wtedy z listy modułów) i przez `RefreshSensors` obiektu ONE_WIRE_HUB (tylko dodaje; po
  zakończeniu OnRefreshCompleted, nowe czujniki pojawią się w OM po discovery). Pomiar wszystkich
  czujników co 2 s.
- ONEW_SENSOR: `Value` w °C, `Threshold` (histereza zdarzeń OnValueChange / Rise / Lower),
  OnOutOfRange poza `MinValue`–`MaxValue`. `Status`: 0 rozłączony (3 nieudane odczyty z rzędu,
  OnDisconnect), 1 połączony (OnConnect po powrocie), 2 znaleziony, jeszcze nie odczytany, 3 moduł
  nieosiągalny. `HubSerialNumber` = numer modułu ANALOG_DIN, `SensorSerialNumber` = 48-bitowy numer
  z ROM.
- ONE_WIRE_HUB: `ConfiguredSensors` (czujniki tego huba w projekcie), `ActiveSensors` (z nich
  połączone), `InactiveSensors`, `ReportedSensors` (znalezione przy ostatnim przeszukaniu).
- Format ramek prawdziwego ANALOG_DIN nie jest znany: firmware używa własnych rekordów (opisanych w
  `components/clu_proto/include/tfbus_vmod.h`) tylko między swoimi modułami a sterownikiem.
  Prawdziwy moduł ANALOG_DIN na RS485 nie jest obsługiwany (sterownik wysyła mu tylko polecenie
  `RefreshSensors`).

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
8 relays + 8 inputs on a single PCF8575 expander, plus 5 additional GPIO inputs, 4 analog inputs and
1-Wire temperature sensors. Board `KinCony F8` in menuconfig (**Board**), preset `f8`
(see [Building](../../README.md)). Other boards: [Supported devices](../DEVICES.md#supported-devices).

- [KinCony F8 — product page](https://www.kincony.com/kincony-f8-esp32-s3-16a-removable-relay-board.html)
- [KinCony F8 — ESPHome Devices](https://devices.esphome.io/devices/kincony-f8/)

Tested on hardware: relays and inputs on PCF8575, GPIO inputs (1-wire terminals and
spare GPIO), and the display all work. Analog inputs and 1-Wire sensors
([below](#analog-inputs-and-1-wire-sensors)) are not tested on hardware yet.

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
| Analog inputs A1 / A2 (0–5 V), A3 / A4 (4–20 mA) | 5 / 7 / 6 / 4 | `CLU_LOCAL_IO_ANALOG_INPUTS` |
| 1-wire terminals 48 / 47 / 21 / 15: DIN1–DIN4 or 1-Wire bus | DIN | `CLU_LOCAL_IO_F8_DIN1_MODE` … `DIN4_MODE` |
| SSD1306 128x64 display: address | 0x3C | `CLU_DISPLAY_ADDR` |
| Setup button | 0 (BOOT) | `CLU_SETUP_BUTTON_GPIO` |

- **Board relays and inputs** appear in OM as a single **IO_MODULE_DIN_8** module (8 DOUT + 8 DIN;
  channel 1 = relay 1 / DI1). The firmware answers TF-Bus frames for this module itself, so it
  participates in discovery, addressing, and polling just like RS485 modules and is included in
  `tfbusDevices` in `a:\CONFIG.JSON`. Add it to your OM project like any discovered module.
- **GPIO inputs** (4 1-wire terminals GPIO48, 47, 21, 15 with onboard pull-ups and spare terminal
  GPIO14 without pull-up) form a second **IO_MODULE_DIN_8** module: DIN1–DIN5 in that order,
  DIN6–DIN8 always inactive. Its DOUT outputs control nothing. The firmware enables internal
  pull-ups; an input is active when shorted to GND. The terminal chosen for the 1-Wire bus
  (`CLU_LOCAL_IO_F8_DINx_MODE`) keeps its channel but is always inactive. An empty list in `CLU_LOCAL_IO_GPIO_INPUTS` (`Custom` board) disables this module.
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
- **Manual relay switches** are visible in OM: a relay switched on by hand has `Value` = 1 in its
  DOUT and fires OnSwitchOn (switching it back off: OnSwitchOff). The firmware reads back the
  PCF8575 relay pins (sampled and debounced like the inputs). A switch is only seen while the CLU
  holds that relay off; a relay the CLU switched on always reads as on. Switching off from the CLU
  a relay held on by its switch changes nothing (the relay stays on), but `Value` reads 0 right
  away until the switch changes state.
- W5500 uses GPIO43/44 (UART0 console pins), so **console and logs go over native USB
  (USB-C port, USB-Serial-JTAG)**; firmware flashing also uses it.
- DS3231 RTC (0x68) is on the same I2C bus as PCF8575; the profile enables it
  (`CLU_RTC_CHIP` = DS3231, SDA 8 / SCL 18). Time survives power loss only with a backup battery
  installed.
- WiFi is disabled by default and can be enabled alongside Ethernet.
- Unused: SD card, EEPROM, 433 MHz RF, Tuya module.
- RS485 without DE pin: see the note on auto-direction transceivers in
  [Other ESP32-S3 boards](other-esp32-s3.md#rs485-transceiver).

## Analog inputs and 1-Wire sensors

They appear in OM as an **ANALOG_DIN** module (like the Grenton ANALOG IN/OUT 2.0) and one
**ONE_WIRE** module per sensor found. The firmware answers their TF-Bus frames itself, so they are
listed in `tfbusDevices`; add them to the project like any discovered module. The ANALOG_DIN module
exists when analog inputs or the 1-Wire bus are configured; its serial is kept in NVS (`sn3`,
overridden by `CLU_LOCAL_IO_SN3`).

- **AnalogIN1–AnalogIN4** = A1–A4. `RawValue` is the value at the terminal: A1/A2 in volts (0–5 V,
  pin voltage × 1.51), A3/A4 in milliamps (4–20 mA, voltage across 150 Ω × 6.667). The factors are
  in `CLU_LOCAL_IO_ANALOG_INPUTS` (`GPIO:factor`). `Value` = `RawValue` × `Scale`; `Value%` runs
  from `MinValue` to `MaxValue`. `Sensitivity` (in `RawValue` units) is the change that fires
  OnValueChange / OnValueLower / OnValueRise. Leaving `MinValue`–`MaxValue` fires OnOutOfRange,
  returning fires OnInRange; reaching `MaxValue` / `MinValue` fires OnSwitchOn / OnSwitchOff. Inputs
  are sampled every 100 ms (average of 16 ADC1 readings, 12 dB attenuation, factory calibration), so
  readings are approximate; A1/A2 saturate at about 4.7 V.
- **AnalogIN5–AnalogIN7** always read 0, **AnalogOUT1–4** drive nothing (the board has no analog
  outputs): features keep the values set, methods do nothing.
- **1-Wire**: in menuconfig set each 1-wire terminal to **Digital input DINx** or **1-Wire bus**
  (`CLU_LOCAL_IO_F8_DIN1_MODE` … `DIN4_MODE`; GPIO48 / 47 / 21 / 15 = DIN1–DIN4 of the GPIO input
  module, pull-up on the board). Several terminals may be buses (separate runs, e.g. to different
  rooms); a bus terminal's DIN channel always reads inactive. All buses are behind the one
  ONE_WIRE_HUB, 16 sensors in total. Supported sensors: DS18B20 and DS18S20 with their own power supply
  (no parasite power), up to 16. Each sensor is a ONE_WIRE module (hwType 0xff, fwType = family code
  0x28 / 0x10) with a ONEW_SENSOR object; the module serial is the low 31 bits of the sensor's ROM
  serial, so a sensor keeps its identity across restarts and when moved to another board.
- The bus is searched at startup, on OM "Clear CLU" (sensors that are gone then drop out of the
  module list) and by the ONE_WIRE_HUB's `RefreshSensors` (only adds; fires OnRefreshCompleted when
  done, new sensors show up in OM after a discovery). All sensors are measured every 2 s.
- ONEW_SENSOR: `Value` in °C, `Threshold` (hysteresis of OnValueChange / Rise / Lower),
  OnOutOfRange outside `MinValue`–`MaxValue`. `Status`: 0 disconnected (3 failed reads in a row,
  OnDisconnect), 1 connected (OnConnect when it comes back), 2 found, not read yet, 3 module
  unreachable. `HubSerialNumber` = the ANALOG_DIN module's serial, `SensorSerialNumber` = the 48-bit
  ROM serial.
- ONE_WIRE_HUB: `ConfiguredSensors` (this hub's sensors in the project), `ActiveSensors` (of those
  connected), `InactiveSensors`, `ReportedSensors` (found by the last search).
- The real ANALOG_DIN's frame format is not known: the firmware uses its own records (described in
  `components/clu_proto/include/tfbus_vmod.h`) only between its modules and its driver. A real
  ANALOG_DIN module on RS485 is not supported (the driver only sends it `RefreshSensors`).

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
