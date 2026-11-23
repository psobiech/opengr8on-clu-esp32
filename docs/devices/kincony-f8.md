# KinCony F8

ESP32-S3-WROOM-1U N16R8 (16 MB flash, 8 MB PSRAM), Ethernet W5500 na SPI, RS485 i
8 przekaźników + 8 wejść na jednym ekspanderze PCF8575. Płytka `KinCony F8` w menuconfig (**Board**), preset `f8`
(zob. [Budowanie](../../README.md#budowanie)). Pozostałe płytki: [Obsługiwane urządzenia](../DEVICES.md).

- [KinCony F8 — strona produktu](https://www.kincony.com/kincony-f8-esp32-s3-16a-removable-relay-board.html)

| Funkcja | GPIO / wartość | Opcja |
|---|---|---|
| W5500: SCLK / MOSI / MISO / CS | 42 / 43 / 44 / 41 | `CLU_ETH_SPI_*` |
| W5500: INT / RST | 2 / 1 | `CLU_ETH_SPI_INT_GPIO`, `CLU_ETH_SPI_RST_GPIO` |
| RS485 TX / RX (bez DE, automatyczny kierunek) | 16 / 17 | `CLU_RS485_*` |
| I2C SDA / SCL | 8 / 18 | `CLU_LOCAL_IO_SDA_GPIO`, `CLU_LOCAL_IO_SCL_GPIO` |
| PCF8575: adres | 0x24 | `CLU_LOCAL_IO_ADDR` |
| PCF8575: przekaźniki / wejścia | P10–P17 / P00–P07 | `CLU_LOCAL_IO_OUT_PINS` |
| Przycisk konfiguracji | 0 (BOOT) | `CLU_SETUP_BUTTON_GPIO` |

- **Przekaźniki i wejścia płytki** widać w OM jako jeden moduł **IO_MODULE_DIN_8** (8 DOUT + 8 DIN;
  kanał 1 = przekaźnik 1 / DI1). Firmware sam odpowiada na ramki TF-Bus tego modułu, więc bierze
  on udział w wykrywaniu, adresowaniu i odpytywaniu jak moduły na RS485 i trafia do
  `tfbusDevices` w `a:\CONFIG.JSON`. Dodaj go do projektu w OM jak każdy wykryty moduł.
- Numer seryjny modułu jest losowany przy pierwszym starcie i zapisywany w NVS (przestrzeń
  `localio`), jak numer CLU; `CLU_LOCAL_IO_SN` w menuconfig go nadpisuje.
- Wyjścia i wejścia są aktywne stanem niskim (`CLU_LOCAL_IO_OUT_ACTIVE_LOW` /
  `IN_ACTIVE_LOW`), jak w konfiguracjach ESPHome producenta; na F8 niesprawdzone na sprzęcie
  (na [KC868-A8v3](kincony-kc868-a8v3.md) sprawdzone).
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
- Nieużywane: wejścia analogowe, 1-wire, karta SD, EEPROM, wyświetlacz, RF 433 MHz, moduł Tuya.
- RS485 bez pinu DE: zob. uwagę o transceiverach z automatycznym kierunkiem w
  [Inne płytki ESP32-S3](other-esp32-s3.md#transceiver-rs485).
