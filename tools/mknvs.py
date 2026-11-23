#!/usr/bin/env python3
"""Build the settings image (partition "cfg") for a prebuilt opengr8on CLU firmware.

The "cfg" partition (0x12000, 12 KB) only holds these settings; the firmware reads it and never
writes it. Its values win over the defaults compiled in (menuconfig empty), so the release
binaries can be used without rebuilding. Everything in namespace "cfg":

    sn (u32)                         - CLU serial number (decimal)
    pin (str)                        - CLU private key / PIN (8 characters, from the CLU sticker)
    wifi_ssid, wifi_pass             - WiFi station (boards with WiFi)
    mqtt_uri, mqtt_user, mqtt_pass   - MQTT broker (empty = menuconfig value)
    ota_pass                         - password for network updates (POST /ota, 8..64 bytes)

Options left out are not written: the firmware then uses its compiled-in default (WiFi / MQTT),
or keeps the serial / PIN it has (random on first boot, shown in the boot log).

Flashing the image replaces only the "cfg" partition: the runtime state in "nvs" (project keys
from Object Manager, static IP, TF-Bus module list...) stays. A serial or PIN different from the
current one resets the project keys to the factory key (OM needs req_set_key again).

Requires the ESP-IDF NVS generator: pip install esp-idf-nvs-partition-gen
(already installed in an ESP-IDF environment).

Example:
    tools/mknvs.py --serial 221010155 --pin ABCD1234 --wifi-ssid HomeNet --wifi-pass secret123 \\
        --mqtt-uri mqtt://192.168.1.10:1883 -o cfg.bin
    esptool --port /dev/ttyUSB0 write-flash 0x12000 cfg.bin
"""
import argparse
import csv
import os
import subprocess
import sys
import tempfile

CFG_OFFSET = 0x12000  # partition "cfg" in partitions.csv / partitions_4mb.csv
CFG_SIZE = 0x3000


def serial(text):
    try:
        serial_number = int(text, 10)
    except ValueError:
        raise argparse.ArgumentTypeError("serial must be a decimal number")
    if not 0 < serial_number <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("serial must be 1..4294967295")
    return serial_number


def pin(text):
    if len(text) != 8 or not text.isascii() or not text.isprintable():
        raise argparse.ArgumentTypeError("PIN / private key must be exactly 8 printable ASCII characters")
    return text


def limited(name, min_bytes, max_bytes):
    def check(text):
        if not min_bytes <= len(text.encode()) <= max_bytes:
            raise argparse.ArgumentTypeError(f"{name} must be {min_bytes}..{max_bytes} bytes")
        return text
    return check


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0],
                                     epilog="See the README section on flashing prebuilt firmware.")
    parser.add_argument("--serial", type=serial, help="CLU serial number (decimal)")
    parser.add_argument("--pin", type=pin, help="CLU private key / PIN (8 characters)")
    parser.add_argument("--wifi-ssid", type=limited("WiFi SSID", 1, 32))
    parser.add_argument("--wifi-pass", type=limited("WiFi password", 8, 63))
    parser.add_argument("--mqtt-uri", help="mqtt://host:1883, mqtts://host:8883, ws://..., wss://... ('' = menuconfig value)")
    parser.add_argument("--mqtt-user")
    parser.add_argument("--mqtt-pass")
    parser.add_argument("--ota-pass", type=limited("OTA password", 8, 64),
                    help="enables firmware updates over HTTP (POST /ota) with this password")
    parser.add_argument("-o", "--output", default="cfg.bin", help="output image (default: cfg.bin)")
    parser.add_argument("--csv", help="also write the generator CSV here (contains the secrets in clear text)")
    args = parser.parse_args()

    cfg = []
    if args.serial is not None:
        cfg.append(["sn", "data", "u32", str(args.serial)])
    if args.pin is not None:
        cfg.append(["pin", "data", "string", args.pin])
    cfg += [[k, "data", "string", v] for k, v in (
        ("wifi_ssid", args.wifi_ssid), ("wifi_pass", args.wifi_pass),
        ("mqtt_uri", args.mqtt_uri), ("mqtt_user", args.mqtt_user), ("mqtt_pass", args.mqtt_pass),
        ("ota_pass", args.ota_pass),
    ) if v is not None]
    if not cfg:
        parser.error("nothing to store: give at least one option")

    rows = [["key", "type", "encoding", "value"], ["cfg", "namespace", "", ""]] + cfg

    out = os.path.abspath(args.output)
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "nvs.csv")
        with open(path, "w", newline="") as csv_file:
            csv.writer(csv_file).writerows(rows)
        if args.csv:
            with open(args.csv, "w", newline="") as csv_file:
                csv.writer(csv_file).writerows(rows)
        result = subprocess.run([sys.executable, "-m", "esp_idf_nvs_partition_gen", "generate",
                                 "--outdir", tmp, path, out, hex(CFG_SIZE)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stdout)
        if "No module named" in result.stdout:
            sys.stderr.write("\ninstall the generator: pip install esp-idf-nvs-partition-gen\n")
        return 1
    print(f"{out}: {', '.join(row[0] for row in cfg)}")
    print(f"flash:  esptool --port <PORT> write-flash {CFG_OFFSET:#x} {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
