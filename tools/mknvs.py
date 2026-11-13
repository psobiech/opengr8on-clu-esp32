#!/usr/bin/env python3
"""Build an NVS partition image that configures a prebuilt opengr8on CLU firmware.

Values stored in NVS win over the defaults compiled in (menuconfig), so the release binaries
can be used without rebuilding:

    namespace "clu": sn (u32)  - CLU serial number (decimal)
                     pin (str) - CLU private key / PIN (8 characters, from the CLU sticker)
    namespace "cfg": wifi_ssid, wifi_pass            - WiFi station (boards with WiFi)
                     mqtt_uri, mqtt_user, mqtt_pass  - MQTT broker (empty URI = MQTT off)

Options left out are not written: the firmware then uses its compiled-in default (WiFi / MQTT)
or generates a random serial / PIN on first boot (shown in the boot log).

Flashing the image replaces the whole NVS partition (0x9000, 0x6000 bytes): project keys set by
Object Manager, the TF-Bus module list and other runtime state are reset.

Requires the ESP-IDF NVS generator: pip install esp-idf-nvs-partition-gen
(already installed in an ESP-IDF environment).

Example:
    tools/mknvs.py --serial 221010155 --pin ABCD1234 --wifi-ssid HomeNet --wifi-pass secret123 \\
        --mqtt-uri mqtt://192.168.1.10:1883 -o nvs.bin
    esptool --port /dev/ttyUSB0 write-flash 0x9000 nvs.bin
"""
import argparse
import csv
import os
import subprocess
import sys
import tempfile

NVS_OFFSET = 0x9000
NVS_SIZE = 0x6000  # partitions.csv / partitions_4mb.csv


def serial(v):
    try:
        n = int(v, 10)
    except ValueError:
        raise argparse.ArgumentTypeError("serial must be a decimal number")
    if not 0 < n <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("serial must be 1..4294967295")
    return n


def pin(v):
    if len(v) != 8 or not v.isascii() or not v.isprintable():
        raise argparse.ArgumentTypeError("PIN / private key must be exactly 8 printable ASCII characters")
    return v


def limited(name, lo, hi):
    def check(v):
        if not lo <= len(v.encode()) <= hi:
            raise argparse.ArgumentTypeError(f"{name} must be {lo}..{hi} bytes")
        return v
    return check


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                 epilog="See the README section on flashing prebuilt firmware.")
    ap.add_argument("--serial", type=serial, help="CLU serial number (decimal)")
    ap.add_argument("--pin", type=pin, help="CLU private key / PIN (8 characters)")
    ap.add_argument("--wifi-ssid", type=limited("WiFi SSID", 1, 32))
    ap.add_argument("--wifi-pass", type=limited("WiFi password", 8, 63))
    ap.add_argument("--mqtt-uri", help="mqtt://host:1883, mqtts://host:8883, ws://..., wss://... ('' = MQTT off)")
    ap.add_argument("--mqtt-user")
    ap.add_argument("--mqtt-pass")
    ap.add_argument("-o", "--output", default="nvs.bin", help="output image (default: nvs.bin)")
    ap.add_argument("--csv", help="also write the generator CSV here (contains the secrets in clear text)")
    a = ap.parse_args()

    clu = []
    if a.serial is not None:
        clu.append(["sn", "data", "u32", str(a.serial)])
    if a.pin is not None:
        clu.append(["pin", "data", "string", a.pin])
    cfg = [[k, "data", "string", v] for k, v in (
        ("wifi_ssid", a.wifi_ssid), ("wifi_pass", a.wifi_pass),
        ("mqtt_uri", a.mqtt_uri), ("mqtt_user", a.mqtt_user), ("mqtt_pass", a.mqtt_pass),
    ) if v is not None]
    if not clu and not cfg:
        ap.error("nothing to store: give at least one option")

    rows = [["key", "type", "encoding", "value"]]
    if clu:
        rows += [["clu", "namespace", "", ""]] + clu
    if cfg:
        rows += [["cfg", "namespace", "", ""]] + cfg

    out = os.path.abspath(a.output)
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "nvs.csv")
        with open(path, "w", newline="") as f:
            csv.writer(f).writerows(rows)
        if a.csv:
            with open(a.csv, "w", newline="") as f:
                csv.writer(f).writerows(rows)
        r = subprocess.run([sys.executable, "-m", "esp_idf_nvs_partition_gen", "generate",
                            "--outdir", tmp, path, out, hex(NVS_SIZE)],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stdout)
        if "No module named" in r.stdout:
            sys.stderr.write("\ninstall the generator: pip install esp-idf-nvs-partition-gen\n")
        return 1
    print(f"{out}: {', '.join(row[0] for row in clu + cfg)}")
    print(f"flash:  esptool --port <PORT> write-flash {NVS_OFFSET:#x} {a.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
