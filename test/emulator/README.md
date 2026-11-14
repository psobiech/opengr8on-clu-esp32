# ESP32 emulator integration check

The emulator job boots the real ESP32-S3 firmware under Espressif's `esp-emu`, complementing the
host tests that replace ESP-IDF with stubs. It tests both the standard 16 MB flash / 8 MB PSRAM
profile and a low-memory 4 MB flash / 2 MB PSRAM profile, checking the reported memory sizes, FAT
storage mount, HTTP listener startup, and Lua runtime's fresh-storage idle state.
A blank `storage` partition has no `MAIN.LUA`, so the boot check expects
`no /fs/a/MAIN.LUA, idle` rather than a user-script `SYSTEM.Init()` marker.

The job also starts two isolated emulator guests, provisions a shared project key, uploads and
read-back verifies a Lua peer script on each device, and tests native encrypted CLU UDP three
times in each direction. A localhost relay forwards the ciphertext unchanged; the check validates
request/reply session IDs, exact nonce-tagged Lua payloads, and that a deliberately dropped request
returns the firmware's timeout error. This covers unicast communication, not LAN broadcast
discovery or physical buses.

The HTTP check also starts two fresh peers and uploads a Lua script using `HttpListener` (class 120)
and `HttpRequest` (class 121). Each direction sends three nonce-tagged POST bodies and verifies the
exact ACK body and HTTP 200 status; each requester also checks an unmatched path returns HTTP 404
without invoking the listener. TCP port 80 is forwarded to host ports 19301 and 19302, which the
other guest reaches through the emulator's `192.168.4.1` host gateway.
Only explicitly idempotent Lua control queries retry once after a lost UDP reply; state-changing
operations such as `http_start()` remain single-shot.

The checks are organized by purpose: `boot/` contains startup validation, `udp/` contains native
CLU UDP coverage, and `http/` contains HTTP object coverage. Each scenario folder keeps its runner,
Lua peer script (where applicable), and focused unit tests together.

## CI behavior

`.github/workflows/firmware.yml` builds `gate-http` and `gate-http-low-memory` with ESP-IDF v6.0.2;
the emulator matrix downloads each matching firmware artifact rather than compiling in the test job.
The low-memory preset uses `partitions_4mb.csv` and quad PSRAM, with its generated `sdkconfig` kept
inside its separate build directory. The low-memory test artifact is not included in GitHub releases.
CI installs `esp-emu` v1.0.0 after checking the published Linux archive SHA-256, then runs the bounded
boot, UDP, and HTTP checks for both profiles. Both pair runners require matching firmware/ELF files
and two generated `cfg` partitions with distinct serial numbers, the same factory PIN, and the
emulator's default test AP credentials (`myssid` / `mypassword`). Each run uses a fresh random
project key and nonce.
Each preset's build log and emulator logs, Lua scripts, UDP packet evidence, and result summaries
(when complete) are uploaded even when a check fails. The UDP relay reserves localhost port 1234;
the guests use UDP control host-forward ports 19201 and 19202. The HTTP run additionally reserves
TCP ports 19301 and 19302. The runners have independent emulator and Unix wall-clock limits.

## Local run

With ESP-IDF v6.0.2 and `esp-emu` v1.0.0 installed:

```sh
idf.py --preset gate-http build merge-bin
python3 tools/mknvs.py --serial 221010155 --pin ABCD1234 --wifi-ssid myssid --wifi-pass mypassword --mqtt-uri "" -o build-gate-http/udp-cfg-a.bin
python3 tools/mknvs.py --serial 221010156 --pin ABCD1234 --wifi-ssid myssid --wifi-pass mypassword --mqtt-uri "" -o build-gate-http/udp-cfg-b.bin
python3 -m pip install cryptography==44.0.2
python3 -m test.emulator.boot.run_boot \
  --firmware build-gate-http/merged-binary.bin \
  --elf build-gate-http/opengr8on_clu_esp32.elf \
  --log build-gate-http/esp-emu.log
python3 -m test.emulator.udp.run_udp \
  --firmware build-gate-http/merged-binary.bin \
  --elf build-gate-http/opengr8on_clu_esp32.elf \
  --cfg-a build-gate-http/udp-cfg-a.bin \
  --cfg-b build-gate-http/udp-cfg-b.bin \
  --out-dir build-gate-http/udp-test
python3 -m test.emulator.http.run_http \
  --firmware build-gate-http/merged-binary.bin \
  --elf build-gate-http/opengr8on_clu_esp32.elf \
  --cfg-a build-gate-http/udp-cfg-a.bin \
  --cfg-b build-gate-http/udp-cfg-b.bin \
  --out-dir build-gate-http/http-test
idf.py --preset gate-http-low-memory build merge-bin
python3 tools/mknvs.py --serial 221010155 --pin ABCD1234 --wifi-ssid myssid --wifi-pass mypassword --mqtt-uri "" -o build-gate-http-low-memory/udp-cfg-a.bin
python3 tools/mknvs.py --serial 221010156 --pin ABCD1234 --wifi-ssid myssid --wifi-pass mypassword --mqtt-uri "" -o build-gate-http-low-memory/udp-cfg-b.bin
python3 -m test.emulator.boot.run_boot \
  --firmware build-gate-http-low-memory/merged-binary.bin \
  --elf build-gate-http-low-memory/opengr8on_clu_esp32.elf \
  --flash-size 4MB --psram-size 2M \
  --log build-gate-http-low-memory/esp-emu.log
python3 -m test.emulator.udp.run_udp \
  --firmware build-gate-http-low-memory/merged-binary.bin \
  --elf build-gate-http-low-memory/opengr8on_clu_esp32.elf \
  --cfg-a build-gate-http-low-memory/udp-cfg-a.bin \
  --cfg-b build-gate-http-low-memory/udp-cfg-b.bin \
  --psram-size 2M --out-dir build-gate-http-low-memory/udp-test
python3 -m test.emulator.http.run_http \
  --firmware build-gate-http-low-memory/merged-binary.bin \
  --elf build-gate-http-low-memory/opengr8on_clu_esp32.elf \
  --cfg-a build-gate-http-low-memory/udp-cfg-a.bin \
  --cfg-b build-gate-http-low-memory/udp-cfg-b.bin \
  --psram-size 2M --out-dir build-gate-http-low-memory/http-test
python3 -m unittest discover -s test -t . -v
```
