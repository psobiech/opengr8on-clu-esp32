#!/usr/bin/env python3
"""Drive a runaway MAIN.LUA into task watchdog resets on one esp-emu guest and check the recovery.

The script busy-loops core 1 (`while true do end`): the Lua task keeps checking in, but IDLE1
starves, so the task watchdog panics and the chip reboots. Each reboot must report the previous
crash from RTC memory (main/clu_crash.c), and after two early Lua crashes in a row the firmware
must not run MAIN.LUA again: it is kept as BAD_M.LUA and the CLU goes to emergency mode.
"""

import argparse
import json
import re
import secrets
import socket
import sys
import time
from pathlib import Path

from ..udp.run_udp import Device, run_with_timeout


UDP_PORT = 19201
CONTROL_PORT = 19501
WATCHDOG_MARKER = "Task watchdog got triggered"
# Wall-clock wait for each crash step. With core 1 busy the emulator runs well below real time, and
# esp-emu 1.0.0 times the 20 s task watchdog out at 41 s of emulated time: about 2 minutes of wall
# clock per crash on a fast host, more on CI runners.
CRASH_STEP_TIMEOUT_S = 600
# "task watchdog: <tasks that stopped checking in> | task <running task> | pc ...": the list only.
CRASH_REPORT_PATTERN = re.compile(r"previous boot crashed: task watchdog: [^|\n]*\b(clu_lua|IDLE1)\b")
SKIPPED_MARKER = "main.lua crashed the CLU on the last boots, not run"
EMERGENCY_MARKER = "main.lua failed -> emergency mode"
# The watchdog panics on purpose here; anything else is a real fault.
UNEXPECTED_FAILURE_PATTERN = re.compile(
    r"LoadProhibited|StoreProhibited|InstrFetchProhibited|IllegalInstruction|assert failed|"
    r"assertion failed|Stack canary watchpoint triggered|Lua VM setup:|cannot create Lua state"
)


def runaway_script(nonce: str) -> str:
    """MAIN.LUA that never blocks: only the starved idle task can stop it."""
    return f'print("WDT_LOOP {nonce}")\nwhile true do end\n'


def run_test(args: argparse.Namespace) -> str:
    for path in (args.firmware, args.elf, args.cfg):
        if not path.is_file():
            raise FileNotFoundError(f"required file does not exist: {path}")
    args.out_dir.mkdir(parents=True, exist_ok=True)

    nonce = secrets.token_hex(8)
    loop_marker = f"WDT_LOOP {nonce}"
    device = None
    try:
        device = Device(
            "CLU",
            args.udp_port,
            args.cfg,
            args.firmware,
            args.elf,
            args.emulator,
            args.out_dir,
            args.emulator_timeout,
            psram_size=args.psram_size,
            extra_emulator_args=("--no-panic-intercept", "--control-tcp", f"127.0.0.1:{args.control_port}"),
        )
        device.wait_for("main: WiFi IP 192.168.4.2")
        device.wait_for("no /fs/a/MAIN.LUA, idle")
        device.discover()
        device.set_project_key(secrets.token_bytes(16), secrets.token_bytes(16))
        script = runaway_script(nonce)
        (args.out_dir / "MAIN.LUA").write_text(script, encoding="utf-8")
        store_main_script(device, script)

        # A cold boot (power-on) starts the crash count from zero.
        reset_chip(args.control_port)
        wait_for_count(device, loop_marker, 1, timeout=CRASH_STEP_TIMEOUT_S)
        wait_for_count(device, WATCHDOG_MARKER, 1, timeout=CRASH_STEP_TIMEOUT_S)
        wait_for_count(device, loop_marker, 2, timeout=CRASH_STEP_TIMEOUT_S)
        wait_for_count(device, WATCHDOG_MARKER, 2, timeout=CRASH_STEP_TIMEOUT_S)
        device.wait_for(SKIPPED_MARKER, timeout=CRASH_STEP_TIMEOUT_S)
        device.wait_for(EMERGENCY_MARKER, timeout=30)

        # Read-only check: safe to retry while the CLU comes back up after the last reboot.
        files = device.call(
            'return tostring(io.open("/fs/a/BAD_M.LUA") ~= nil) .. "," .. tostring(io.open("/fs/a/MAIN.LUA") ~= nil)',
            retries=5,
            timeout=15,
        )
        if files != "true,false":
            raise RuntimeError(f"watchdog test: expected BAD_M.LUA kept and MAIN.LUA gone, got {files!r}")

        validate_watchdog_log(device.log_path.read_text(encoding="utf-8", errors="replace"), nonce)
        if device.process.poll() is not None:
            raise RuntimeError(f"emulator exited early with {device.process.returncode}")
        return (
            "PASS: runaway MAIN.LUA -> task watchdog panic twice, each crash reported on the next boot, "
            f"then MAIN.LUA skipped (BAD_M.LUA, emergency mode). nonce={nonce}"
        )
    finally:
        if device is not None:
            device.close()


def store_main_script(device: Device, script: str) -> None:
    """Write MAIN.LUA without running it (Device.upload() would run the loop inside a req: call)."""
    literal = json.dumps(script)  # ASCII only: a JSON string is a valid Lua string literal
    stored = device.call(
        f'local f=assert(io.open("/fs/a/MAIN.LUA","wb")); assert(f:write({literal})); assert(f:close()); '
        'local r=assert(io.open("/fs/a/MAIN.LUA","rb")); local s=r:read("a"); r:close(); return #s',
        retries=1,
    )
    if stored != str(len(script)):
        raise RuntimeError(f"watchdog test: stored MAIN.LUA length {stored!r}, expected {len(script)}")


def reset_chip(control_port: int, timeout: float = 15) -> None:
    """Hard reset through esp-emu's control channel (what a DTR/RTS toggle does on hardware)."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            with socket.create_connection(("127.0.0.1", control_port), timeout=5) as connection:
                connection.sendall(b"reset\n")
                reply = connection.makefile("r", encoding="utf-8").readline().strip()
            if reply != "ok":
                raise RuntimeError(f"watchdog test: control reset answered {reply!r}")
            return
        except ConnectionRefusedError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.2)


def wait_for_count(device: Device, marker: str, count: int, timeout: float) -> None:
    """Wait until @p marker appears at least @p count times in the emulator log."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if device.log_path.read_text(encoding="utf-8", errors="replace").count(marker) >= count:
            return
        if device.process.poll() is not None:
            raise RuntimeError(f"emulator exited with {device.process.returncode}; see {device.log_path}")
        time.sleep(0.2)
    raise TimeoutError(f"timed out waiting for {marker!r} x{count}; see {device.log_path}")


def validate_watchdog_log(output: str, nonce: str) -> None:
    """Two watchdog crashes of the runaway script, each reported, then MAIN.LUA skipped once."""
    loop_marker = f"WDT_LOOP {nonce}"
    if output.count(loop_marker) != 2:
        raise ValueError(f"expected the runaway script to start exactly twice, saw {output.count(loop_marker)}")
    if output.count(WATCHDOG_MARKER) != 2:
        raise ValueError(f"expected exactly two task watchdog timeouts, saw {output.count(WATCHDOG_MARKER)}")
    crash_reports = CRASH_REPORT_PATTERN.findall(output)
    if len(crash_reports) != 2:
        raise ValueError(f"expected both watchdog crashes reported on the next boot, saw {len(crash_reports)}")
    if output.count(SKIPPED_MARKER) != 1:
        raise ValueError(f"expected MAIN.LUA skipped exactly once, saw {output.count(SKIPPED_MARKER)}")
    second_report = [match.start() for match in CRASH_REPORT_PATTERN.finditer(output)][1]
    if not second_report < output.index(SKIPPED_MARKER) < output.index(EMERGENCY_MARKER):
        raise ValueError("MAIN.LUA must be skipped after the second crash report, then emergency mode")
    match = UNEXPECTED_FAILURE_PATTERN.search(output)
    if match:
        raise ValueError(f"emulator log contains failure signature {match.group(0)!r}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", default="esp-emu", help="esp-emu executable")
    parser.add_argument("--firmware", type=Path, required=True, help="merged flash image")
    parser.add_argument("--elf", type=Path, required=True, help="matching application ELF")
    parser.add_argument("--cfg", type=Path, required=True, help="generated 12 KiB cfg partition")
    parser.add_argument("--out-dir", type=Path, required=True, help="directory for emulator logs and evidence")
    parser.add_argument("--psram-size", default="8M", help="emulated PSRAM size (for example 2M or 8M)")
    parser.add_argument("--udp-port", type=int, default=UDP_PORT, help="host UDP port forwarded to the CLU")
    parser.add_argument("--control-port", type=int, default=CONTROL_PORT, help="host TCP port of esp-emu's control channel")
    parser.add_argument("--host-timeout", type=float, default=1500, help="overall wall-clock timeout in seconds")
    parser.add_argument("--emulator-timeout", type=int, default=1440, help="emulator time limit in seconds")
    args = parser.parse_args()
    if args.host_timeout <= 0 or args.emulator_timeout <= 0:
        parser.error("timeouts must be greater than zero")

    try:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        (args.out_dir / "result.txt").write_text("RUNNING\n", encoding="utf-8")
        summary = run_with_timeout(lambda: run_test(args), args.host_timeout, label="watchdog integration")
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
