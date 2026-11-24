# Inne płytki ESP32-S3

Wymagane: ESP32-S3 z 16 MB flash i 8 MB PSRAM (octal), transceiver RS485 3,3 V, zasilanie z 24 V
magistrali przez przetwornicę. W menuconfig wybierz **Board** → `Custom`: wtedy piny RS485, RTC,
Ethernetu i IO są edytowalne. Pozostałe płytki: [Obsługiwane urządzenia](../DEVICES.md).

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

## Transceiver RS485

- Użyj transceivera 3,3 V (MAX3485, SP3485, THVD1450) albo izolowanego (ADM2587E, ISO3082).
  Moduł MAX485 na 5 V wystawia RO na poziomie 5 V, czego GPIO S3 nie tolerują.
- Moduły z automatycznym kierunkiem (bez pinu DE) działają z `CLU_RS485_DE_GPIO=-1`. Wolniej
  przełączają kierunek, więc szybko odpowiadające moduły mogą zostać pominięte.
- Zamontuj terminator 120 Ω po stronie CLU oraz rezystory polaryzujące, jeśli transceiver nie ma
  funkcji fail-safe.
- **Nigdy nie podłączaj dwóch masterów (prawdziwej CLU i tej) do tej samej magistrali.**

---

# Other ESP32-S3 boards

Required: ESP32-S3 with 16 MB flash and 8 MB PSRAM (octal), 3.3 V RS485 transceiver, powered from
the 24 V bus via a buck converter. In menuconfig select **Board** → `Custom`: then the RS485, RTC,
Ethernet, and IO pins become editable. Other boards: [Supported devices](../DEVICES.md#supported-devices).

Example board: [ESP32-S3 N8R8 from Kamami](https://kamami.pl/esp32/1184834-esp32-s3-microcontroller-2-4ghz-wi-fi-development-board-240mhz-dual-core-processor-esp32-s3-wroom-5906623423545.html).
The N8R8 variant has 8 MB flash: for GATE HTTP use the `gate-http-N8R8` preset and release images
ending in `-N8R8` (no RS485). Other ESP32-S3 profiles still require 16 MB flash.

```
 ESP32-S3                3.3 V RS485 transceiver            TFBUS cable
 GPIO17 (TX) ─────────── DI                          A ──── A
 GPIO18 (RX) ─────────── RO                          B ──── B
 GPIO21 (DE) ─────────── DE + /RE (tied)                    GND (isolated side)
 3V3, GND ────────────── VCC, GND                           +24 V → buck → 5 V → board
```

## RS485 transceiver

- Use a 3.3 V transceiver (MAX3485, SP3485, THVD1450) or an isolated one (ADM2587E, ISO3082).
  A 5 V MAX485 module drives RO at 5 V, which ESP32-S3 GPIOs do not tolerate.
- Auto-direction modules (without a DE pin) work with `CLU_RS485_DE_GPIO=-1`. They switch direction
  more slowly, so fast-responding modules may be missed.
- Fit a 120 Ω terminator on the CLU side and biasing resistors if the transceiver lacks fail-safe
  support.
- **Never connect two masters (a real CLU and this one) to the same bus.**
