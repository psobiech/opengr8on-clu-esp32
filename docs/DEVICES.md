# Obsługiwane urządzenia

Płytki, na których działa opengr8on-clu-esp32: piny, opcja menuconfig **Board** i uwagi sprzętowe.
Budowanie i opcje menuconfig: [README](../README.md).

- [Waveshare ESP32-S3-RS485-CAN](devices/waveshare-esp32-s3-rs485-can.md) — zalecana płytka (z zewnętrzną anteną)
- [Inne płytki ESP32-S3](devices/other-esp32-s3.md) — własny transceiver RS485
- [KinCony F8](devices/kincony-f8.md) — ESP32-S3, W5500, 8 przekaźników / 8 wejść
- [KinCony KC868-A8v3](devices/kincony-kc868-a8v3.md) — ESP32-S3, W5500, 8 przekaźników / 8 wejść
- [Grenton GATE](devices/grenton-gate.md) — ESP32-WROVER-B, Ethernet LAN8720A

## Wymagania pamięci

**PSRAM jest wymagany.** Sam runtime Lua, zanim wczyta się projekt OM, zajmuje ok. 170 KB
(biblioteki standardowe, `prelude.lua`, obiekty wirtualne z `objects.lua`, `mqtt.lua`,
`http.lua`), a projekt OM, skrypty i treści HTTP / MQTT dochodzą do tego. ESP32 bez PSRAM (np.
moduły ESP32-WROOM-32) ma po starcie WiFi i usług ok. 65 KB wolnego wewnętrznego RAM, więc Lua się
na nim nie uruchomi (w logu `Lua VM setup: not enough memory`; sieć, OTA i telnet działają dalej).
Obsługiwane płytki mają 8 MB PSRAM. Flash: 4 MB
wystarcza (`partitions_4mb.csv`), na ESP32-S3 profile domyślnie zakładają 16 MB;
`gate-http-N8R8` obsługuje 8 MB flash i 8 MB octal PSRAM (`partitions_8mb.csv`).

---

# Supported devices

Boards on which opengr8on-clu-esp32 runs: pins, **Board** menuconfig option, and hardware notes.
Building and menuconfig options: [README](../README.md).

- [Waveshare ESP32-S3-RS485-CAN](devices/waveshare-esp32-s3-rs485-can.md) — recommended board (with external antenna)
- [Other ESP32-S3 boards](devices/other-esp32-s3.md) — custom RS485 transceiver
- [KinCony F8](devices/kincony-f8.md) — ESP32-S3, W5500, 8 relays / 8 inputs
- [KinCony KC868-A8v3](devices/kincony-kc868-a8v3.md) — ESP32-S3, W5500, 8 relays / 8 inputs
- [Grenton GATE](devices/grenton-gate.md) — ESP32-WROVER-B, LAN8720A Ethernet

## Memory requirements

**PSRAM is required.** The Lua runtime alone, before loading the OM project, uses approx. 170 KB
(standard libraries, `prelude.lua`, virtual objects from `objects.lua`, `mqtt.lua`, `http.lua`),
plus the OM project, scripts, and HTTP / MQTT payloads. An ESP32 without PSRAM (e.g.
ESP32-WROOM-32 modules) has approx. 65 KB of free internal RAM after WiFi and services start, so Lua
will not start on it (`Lua VM setup: not enough memory` in the log; networking, OTA, and telnet
keep running). Supported boards have 8 MB PSRAM. 4 MB flash is enough (`partitions_4mb.csv`);
ESP32-S3 profiles assume 16 MB by default; `gate-http-N8R8` supports 8 MB flash and 8 MB octal
PSRAM (`partitions_8mb.csv`).
