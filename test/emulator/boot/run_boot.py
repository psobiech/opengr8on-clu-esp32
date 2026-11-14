#!/usr/bin/env python3
"""Boot the gate-http merged image under esp-emu and check startup readiness."""

import argparse
import re
import subprocess
import sys
from pathlib import Path


def boot_markers(flash_size: str = "16MB", psram_size: str = "8M") -> tuple[str, ...]:
    psram_megabytes = psram_size.removesuffix("M")
    return (
        f"SPI Flash Size : {flash_size}",
        f"Found {psram_megabytes}MB PSRAM device",
        "storage mounted at /fs",
        "HttpListener on TCP port 80",
        "no /fs/a/MAIN.LUA, idle",
    )


BOOT_MARKERS = boot_markers()
LUA_READY_MARKER = BOOT_MARKERS[-1]
FAILURE_PATTERNS = (
    re.compile(
        r"Guru Meditation|Firmware abort|abort\(\) was called|assert failed|assertion failed|"
        r"panicked by|rebooting|Stack canary watchpoint triggered|Task watchdog got triggered|"
        r"LoadProhibited|StoreProhibited|InstrFetchProhibited",
        re.IGNORECASE,
    ),
    re.compile(r"Lua VM setup:|cannot create Lua state|SYSTEM\.Loop:"),
    re.compile(r"not from any of the .* application ELF\(s\) given"),
)


def assess_boot(
    output: str, returncode: int, flash_size: str = "16MB", psram_size: str = "8M"
) -> list[str]:
    """Return boot-check failures for captured emulator output and its exit code."""
    failures = []
    if returncode != 0:
        failures.append(f"emulator exited with status {returncode}")
    for marker in boot_markers(flash_size, psram_size):
        if marker not in output:
            failures.append(f"missing boot marker: {marker}")
    expected_psram = f"{psram_size.removesuffix('M')}MB"
    for actual_flash in re.findall(r"SPI Flash Size\s*:\s*(\d+MB)", output):
        if actual_flash != flash_size:
            failures.append(f"unexpected flash size marker: {actual_flash}")
    for actual_psram in re.findall(r"Found (\d+MB) PSRAM device", output):
        if actual_psram != expected_psram:
            failures.append(f"unexpected PSRAM size marker: {actual_psram}")
    for pattern in FAILURE_PATTERNS:
        match = pattern.search(output)
        if match:
            failures.append(f"failure signature: {match.group(0)}")
    return failures


def _output_text(output: str | bytes | None) -> str:
    if output is None:
        return ""
    if isinstance(output, bytes):
        return output.decode("utf-8", errors="replace")
    return output


def run_check(
    command: list[str],
    host_timeout: float,
    log_path: Path,
    runner=None,
    flash_size: str = "16MB",
    psram_size: str = "8M",
) -> list[str]:
    """Run the emulator, preserve its full output, and return any check failures."""
    runner = runner or subprocess.run
    process_failures = []
    try:
        completed = runner(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
            timeout=host_timeout,
            check=False,
        )
        output = _output_text(completed.stdout)
        returncode = completed.returncode
    except subprocess.TimeoutExpired as exc:
        output = _output_text(exc.stdout)
        output += f"\n[runner] host timeout after {host_timeout:g} seconds\n"
        returncode = 124
        process_failures.append(f"emulator exceeded host timeout after {host_timeout:g} seconds")
    except OSError as exc:
        output = f"[runner] could not start emulator: {exc}\n"
        returncode = 127
        process_failures.append(f"could not start emulator: {exc}")

    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(output, encoding="utf-8")
    return process_failures + assess_boot(output, returncode, flash_size, psram_size)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", default="esp-emu", help="esp-emu executable")
    parser.add_argument("--firmware", type=Path, required=True, help="merged flash image")
    parser.add_argument("--elf", type=Path, required=True, help="matching application ELF")
    parser.add_argument("--log", type=Path, required=True, help="emulator output log")
    parser.add_argument("--flash-size", choices=("4MB", "16MB"), default="16MB")
    parser.add_argument("--psram-size", default="8M", help="emulated PSRAM size (for example 2M or 8M)")
    parser.add_argument(
        "--host-timeout", type=float, default=90, help="outer process timeout in seconds"
    )
    parser.add_argument(
        "--emulator-timeout", type=int, default=60, help="emulator time limit in seconds"
    )
    args = parser.parse_args()

    for path in (args.firmware, args.elf):
        if not path.is_file():
            print(f"required file does not exist: {path}", file=sys.stderr)
            return 2

    command = [
        args.emulator,
        "--chip",
        "esp32s3",
        "--firmware",
        str(args.firmware),
        "--elf",
        str(args.elf),
        "--psram-size",
        args.psram_size,
        "--net",
        "user",
        "--timeout",
        f"{args.emulator_timeout}s",
        "--exit-on",
        LUA_READY_MARKER,
    ]
    failures = run_check(
        command,
        args.host_timeout,
        args.log,
        flash_size=args.flash_size,
        psram_size=args.psram_size,
    )
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        print(f"See emulator log: {args.log}", file=sys.stderr)
        return 1

    print(f"PASS: gate-http reached storage and Lua idle markers; log: {args.log}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())