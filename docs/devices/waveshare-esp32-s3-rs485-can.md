# Waveshare ESP32-S3-RS485-CAN

Zalecana płytka: **[Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/wiki/ESP32-S3-RS485-CAN)** (ESP32-S3R8, 16 MB flash, 8 MB PSRAM, RS485 na płytce; [sklep Kamami](https://kamami.pl/moduly-wifi/1201081-industrial-esp32-s3-control-board-with-rs485-and-can-communication-interfaces-supports-wi-fi-blue-5902186330269.html)).
Preset `waveshare` (domyślny), zob. [Budowanie](../../README.md#budowanie). Pozostałe płytki:
[Obsługiwane urządzenia](../DEVICES.md).

Piny z przykładów producenta (`WS_GPIO.h`), ustawiane przez płytkę `Waveshare ESP32-S3-RS485-CAN`
w menuconfig (**Board**, domyślna na `esp32s3`):

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

---

# Waveshare ESP32-S3-RS485-CAN

Recommended board: **[Waveshare ESP32-S3-RS485-CAN](https://www.waveshare.com/wiki/ESP32-S3-RS485-CAN)** (ESP32-S3R8, 16 MB flash, 8 MB PSRAM, onboard RS485; [Kamami store](https://kamami.pl/moduly-wifi/1201081-industrial-esp32-s3-control-board-with-rs485-and-can-communication-interfaces-supports-wi-fi-blue-5902186330269.html)).
Preset `waveshare` (default), see [Building](../../README.md). Other boards:
[Supported devices](../DEVICES.md#supported-devices).

Pins from the vendor's examples (`WS_GPIO.h`), set by the `Waveshare ESP32-S3-RS485-CAN` board
in menuconfig (**Board**, default on `esp32s3`):

| Signal | GPIO |
|---|---|
| RS485 TX | 17 |
| RS485 RX | 18 |
| RS485 DE | 21 (driven as UART RTS in `UART_MODE_RS485_HALF_DUPLEX`) |
| CAN TX / RX | 15 / 16 (unused) |
| RTC PCF85063 SDA / SCL | 39 / 38 (INT 40 unused) |

Connect the board's RS485 A/B to TF-Bus A/B and GND.

The RTC sets system time at boot before scripts start, so they have the right time without a
network. Every SNTP sync and `SetDateTime` writes it back. Time survives power loss only with a
backup cell in the RTC battery connector (J7); without it, after power-on the RTC reports the time
as invalid and the clock is wrong until the first SNTP sync.
