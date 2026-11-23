# Grenton GATE

Preset `gate`, target `esp32`, płytka `Grenton GATE` w menuconfig (**Board**; `sdkconfig.defaults.esp32`,
`partitions_4mb.csv`): ESP32-WROVER-B (4 MB flash,
PSRAM), Ethernet przez wewnętrzny EMAC + PHY LAN8720A. Pozostałe płytki:
[Obsługiwane urządzenia](../DEVICES.md). Domyślne piny:

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

## Wgrywanie na GATE z włączonym szyfrowaniem flash

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
