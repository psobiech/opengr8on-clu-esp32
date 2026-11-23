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
- W5500 zajmuje GPIO43/44 (piny konsoli UART0), więc **konsola i log idą przez natywne USB
  (złącze USB-C, USB-Serial-JTAG)**; tamtędy też wgrywa się firmware.
- Zegar RTC DS3231 (0x68) jest na tej samej magistrali I2C co PCF8575; profil go włącza
  (`CLU_RTC_CHIP` = DS3231, SDA 8 / SCL 18). Czas przetrwa zanik zasilania tylko z baterią
  podtrzymującą.
- WiFi domyślnie wyłączone, można włączyć obok Ethernetu.
- Nieużywane: wejścia analogowe, 1-wire, EEPROM, wyświetlacz, moduł Tuya.
- RS485 bez pinu DE: zob. uwagę o transceiverach z automatycznym kierunkiem w
  [Inne płytki ESP32-S3](other-esp32-s3.md#transceiver-rs485).
