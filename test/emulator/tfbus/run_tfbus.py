#!/usr/bin/env python3
"""Drive the real RS485 TF-Bus path of one esp-emu guest against an emulated dimmer module."""

import argparse
import re
import secrets
import sys
import time
from pathlib import Path

from ..udp.run_udp import BOOT_FAILURE_PATTERNS, Device, run_with_timeout
from .fake_module import (
    FRAME_STATUS_POLL,
    RECORD_SET_RAMP_TIME,
    RECORD_SET_VALUE,
    DimmerModule,
    ModuleLink,
)


MODULE_SERIAL_NUMBER = 330000101
UDP_PORT = 19201
UART1_PORT = 19401
TFBUS_FAILURE_PATTERNS = BOOT_FAILURE_PATTERNS + (re.compile(r"TFBUS test:"),)


def run_test(args: argparse.Namespace) -> str:
    for path in (args.firmware, args.elf, args.cfg):
        if not path.is_file():
            raise FileNotFoundError(f"required file does not exist: {path}")
    args.out_dir.mkdir(parents=True, exist_ok=True)

    nonce = secrets.token_hex(8)
    project_key = secrets.token_bytes(16)
    project_iv = secrets.token_bytes(16)
    serial_number = MODULE_SERIAL_NUMBER
    template = Path(__file__).with_name("tfbus_main.lua").read_text(encoding="utf-8")
    module = DimmerModule(serial_number)
    link = None
    device = None
    try:
        # Connects (with retries) before the firmware's startup RESET / SEEK / DISCOVER goes out.
        link = ModuleLink(module, args.uart_port, args.out_dir / "tfbus-frames.jsonl", args.emulator_timeout)
        device = Device(
            "CLU",
            UDP_PORT,
            args.cfg,
            args.firmware,
            args.elf,
            args.emulator,
            args.out_dir,
            args.emulator_timeout,
            psram_size=args.psram_size,
            extra_emulator_args=("--uart1-tcp", f"127.0.0.1:{args.uart_port}"),
        )
        link.wait_until(
            lambda: module.address != 0 and module.frame_counts.get(FRAME_STATUS_POLL, 0) > 0,
            60,
            "address assignment and the first STATUS_POLL",
        )
        device.wait_for(f"DISCOVER: sn={serial_number:08x}")
        device.wait_for("main: WiFi IP 192.168.4.2")
        device.wait_for("no /fs/a/MAIN.LUA, idle")
        device.discover()
        device.set_project_key(project_key, project_iv)
        script = template.replace("@NONCE@", nonce).replace("@SN@", str(serial_number))
        (args.out_dir / "TFBUS.LUA").write_text(script, encoding="utf-8")
        device.upload(script)
        device.wait_for(f"TFBUS_READY {nonce}")

        check_set_value(device, link)
        check_module_report(device, link)
        check_power_cycle(device, link, serial_number)

        validate_tfbus_log(device.log_path.read_text(encoding="utf-8", errors="replace"), nonce)
        if device.process.poll() is not None:
            raise RuntimeError(f"emulator exited early with {device.process.returncode}")
        return (
            f"PASS: TF-Bus over UART1: dimmer sn={serial_number} discovered and addressed, "
            "Value 0.5 sent as SET_RAMP_TIME + SET_VALUE, module level 0.25 reported to Lua, "
            f"power cycle -> OFFLINE, rejoined, level re-sent. nonce={nonce}"
        )
    finally:
        if device is not None:
            device.close()
        if link is not None:
            link.close()


def check_set_value(device: Device, link: ModuleLink) -> None:
    """Lua Value 0.5 -> MODULE_CMD SET_RAMP_TIME then SET_VALUE [0, 500 LE] on the wire."""
    first_command = link.command_count()
    if device.call("return tfbus_set(0.5)") != "set":
        raise RuntimeError("TFBUS test: tfbus_set(0.5) failed")
    link.wait_until(
        lambda: (RECORD_SET_VALUE, 0x00, bytes([0, 0xF4, 0x01])) in link.module.commands[first_command:],
        15,
        "SET_VALUE 500",
    )
    validate_set_value_commands(link.commands_since(first_command), 500)
    read_back = lua_value(device)
    if read_back != 0.5:
        raise RuntimeError(f"TFBUS test: Value read back {read_back}, expected 0.5")


def check_module_report(device: Device, link: ModuleLink) -> None:
    """The module's own level change (status record 01) becomes the Lua Value."""
    link.set_level(250)
    deadline = time.monotonic() + 15  # past the driver's 1 s post-command status hold-off
    while lua_value(device) != 0.25:
        if time.monotonic() >= deadline:
            raise TimeoutError("TFBUS test: Value did not follow the module's level 250")
        time.sleep(0.5)


def check_power_cycle(device: Device, link: ModuleLink, serial_number: int) -> None:
    """Silent module -> OFFLINE; back unaddressed -> rejoined by ADDR_ASSIGN, last level re-sent."""
    address = link.address()
    link.set_powered(False)
    device.wait_for(f"addr {address} sn={serial_number} OFFLINE", timeout=30)
    first_command = link.command_count()
    link.set_powered(True)
    device.wait_for(f"addr {address} sn={serial_number} rejoined", timeout=60)
    link.wait_until(
        lambda: link.module.address == address
        and (RECORD_SET_VALUE, 0x00, bytes([0, 0xFA, 0x00])) in link.module.commands[first_command:],
        15,
        "the last level (250) re-sent after the rejoin",
    )


def lua_value(device: Device) -> float:
    reply = device.call("return DIM0001:get(0)", retries=1)
    try:
        return float(reply)
    except ValueError:
        raise RuntimeError(f"TFBUS test: DIM0001 Value is not a number: {reply!r}") from None


def validate_set_value_commands(commands: list[tuple[int, int, bytes]], level: int) -> None:
    """Require SET_RAMP_TIME on channel 0 before the SET_VALUE record carrying @p level."""
    expected = (RECORD_SET_VALUE, 0x00, bytes([0, level & 0xFF, level >> 8]))
    if expected not in commands:
        raise ValueError(f"no SET_VALUE record for level {level}")
    preceding = commands[: commands.index(expected)]
    if not any(is_channel0_ramp_time(record) for record in preceding):
        raise ValueError(f"SET_VALUE {level} was not preceded by SET_RAMP_TIME on channel 0")


def is_channel0_ramp_time(record: tuple[int, int, bytes]) -> bool:
    """SET_RAMP_TIME per docs/TFBUS.md: type 06, meta 0, data [0, ms LE u16, 0, 0]."""
    record_type, meta, data = record
    return record_type == RECORD_SET_RAMP_TIME and meta == 0 and len(data) == 5 and data[0] == 0 and data[3:] == b"\x00\x00"


def validate_tfbus_log(output: str, nonce: str) -> None:
    """Require the script's markers (OnValueChange for both values) and no failure signature."""
    if output.count(f"TFBUS_READY {nonce}") != 1:
        raise ValueError(f"expected exactly one log marker 'TFBUS_READY {nonce}'")
    for value in ("0.5", "0.25"):
        if f"TFBUS_VALUE {nonce} {value}\n" not in output.replace("\r\n", "\n"):
            raise ValueError(f"missing OnValueChange marker for Value {value}")
    for pattern in TFBUS_FAILURE_PATTERNS:
        match = pattern.search(output)
        if match:
            raise ValueError(f"emulator log contains failure signature {match.group(0)!r}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", default="esp-emu", help="esp-emu executable")
    parser.add_argument("--firmware", type=Path, required=True, help="merged flash image of a TF-Bus build")
    parser.add_argument("--elf", type=Path, required=True, help="matching application ELF")
    parser.add_argument("--cfg", type=Path, required=True, help="generated 12 KiB cfg partition")
    parser.add_argument("--out-dir", type=Path, required=True, help="directory for emulator logs and evidence")
    parser.add_argument("--psram-size", default="8M", help="emulated PSRAM size (for example 2M or 8M)")
    parser.add_argument("--uart-port", type=int, default=UART1_PORT, help="host TCP port of the UART1 bridge")
    parser.add_argument("--host-timeout", type=float, default=240, help="overall wall-clock timeout in seconds")
    parser.add_argument("--emulator-timeout", type=int, default=180, help="emulator time limit in seconds")
    args = parser.parse_args()
    if args.host_timeout <= 0 or args.emulator_timeout <= 0:
        parser.error("timeouts must be greater than zero")

    try:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        (args.out_dir / "result.txt").write_text("RUNNING\n", encoding="utf-8")
        summary = run_with_timeout(lambda: run_test(args), args.host_timeout, label="TF-Bus integration")
        (args.out_dir / "result.txt").write_text(summary + "\n", encoding="utf-8")
        print(summary, flush=True)
        return 0
    except (OSError, RuntimeError, TimeoutError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        print(f"Inspect emulator evidence in {args.out_dir}", file=sys.stderr)
        try:
            (args.out_dir / "result.txt").write_text(f"FAIL: {exc}\n", encoding="utf-8")
        except OSError:
            pass
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
