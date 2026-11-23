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
