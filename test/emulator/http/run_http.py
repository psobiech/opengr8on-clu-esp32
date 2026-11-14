#!/usr/bin/env python3
"""Run bidirectional HttpRequest/HttpListener tests between two esp-emu guests."""

import argparse
import re
import secrets
import sys
from pathlib import Path

from ..udp.run_udp import BOOT_FAILURE_PATTERNS, Device, run_with_timeout


HTTP_FAILURE_PATTERNS = BOOT_FAILURE_PATTERNS + (
    re.compile(r"bad HTTP request|bad HTTP response|bad HTTP 404 response|HTTP test:"),
)


def validate_http_logs(logs: dict[str, str], nonce: str) -> None:
    """Require exact bidirectional request/reply markers and both unmatched-route checks."""
    if set(logs) != {"A", "B"}:
        raise ValueError("expected emulator logs for peers A and B")
    for name, peer in (("A", "B"), ("B", "A")):
        output = logs[name]
        markers = [
            f"HTTP_READY {name} {nonce}",
            f"HTTP_DONE {name} {nonce}",
            f"HTTP_404_OK {name} {nonce}",
        ]
        markers += [f"HTTP_REPLY_OK {name} {seq} {nonce}" for seq in range(1, 4)]
        markers += [f"HTTP_RECEIVED {name} {nonce}:{peer}:post:{seq}" for seq in range(1, 4)]
        for marker in markers:
            if output.count(marker) != 1:
                raise ValueError(f"{name}: expected exactly one log marker {marker!r}")
        for pattern in HTTP_FAILURE_PATTERNS:
            match = pattern.search(output)
            if match:
                raise ValueError(f"{name}: emulator log contains failure signature {match.group(0)!r}")


def run_test(args: argparse.Namespace) -> str:
    for path in (args.firmware, args.elf, args.cfg_a, args.cfg_b):
        if not path.is_file():
            raise FileNotFoundError(f"required file does not exist: {path}")
    args.out_dir.mkdir(parents=True, exist_ok=True)

    devices = []
    nonce = secrets.token_hex(8)
    project_key = secrets.token_bytes(16)
    project_iv = secrets.token_bytes(16)
    template = (Path(__file__).with_name("http_peer.lua")).read_text(encoding="utf-8")
    try:
        for name, udp_port, tcp_port, cfg in (
            ("A", 19201, 19301, args.cfg_a),
            ("B", 19202, 19302, args.cfg_b),
        ):
            devices.append(
                Device(
                    name,
                    udp_port,
                    cfg,
                    args.firmware,
                    args.elf,
                    args.emulator,
                    args.out_dir,
                    args.emulator_timeout,
                    tcp_port=tcp_port,
                    psram_size=getattr(args, "psram_size", "8M"),
                )
            )

        for device in devices:
            device.wait_for("main: WiFi IP 192.168.4.2")
            device.wait_for("no /fs/a/MAIN.LUA, idle")
            device.discover()
            device.set_project_key(project_key, project_iv)
            peer = "B" if device.name == "A" else "A"
            script = template.replace("@NAME@", device.name).replace("@PEER@", peer).replace("@NONCE@", nonce)
            (args.out_dir / f"HTTP-{device.name}.LUA").write_text(script, encoding="utf-8")
            device.upload(script)
            device.wait_for(f"HTTP_READY {device.name} {nonce}")

        for source, target in ((devices[0], devices[1]), (devices[1], devices[0])):
            if source.call(f"return http_start({target.tcp_port})") != "started":
                raise RuntimeError(f"{source.name}: failed to start HttpRequest exchange")
            source.wait_for(f"HTTP_DONE {source.name} {nonce}", timeout=60)
            if source.call("return http_completed()", retries=1) != "3":
                raise RuntimeError(f"{source.name}: not all three HttpRequest responses completed")
            if target.call("return http_received()", retries=1) != "3":
                raise RuntimeError(f"{target.name}: did not receive exactly three HttpListener requests")

        logs = {
            device.name: device.log_path.read_text(encoding="utf-8", errors="replace")
            for device in devices
        }
        validate_http_logs(logs, nonce)
        for device in devices:
            if device.process.poll() is not None:
                raise RuntimeError(f"{device.name}: emulator exited early with {device.process.returncode}")

        return (
            "PASS: HttpRequest/HttpListener A -> B 3/3, B -> A 3/3; exact nonce-tagged POST/ACK bodies "
            f"and unmatched-path HTTP 404 verified. nonce={nonce}"
        )
    finally:
        for device in devices:
            device.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", default="esp-emu", help="esp-emu executable")
    parser.add_argument("--firmware", type=Path, required=True, help="merged flash image")
    parser.add_argument("--elf", type=Path, required=True, help="matching application ELF")
    parser.add_argument("--cfg-a", type=Path, required=True, help="generated 12 KiB cfg partition for peer A")
    parser.add_argument("--cfg-b", type=Path, required=True, help="generated 12 KiB cfg partition for peer B")
    parser.add_argument("--out-dir", type=Path, required=True, help="directory for emulator logs and evidence")
    parser.add_argument("--psram-size", default="8M", help="emulated PSRAM size (for example 2M or 8M)")
    parser.add_argument("--host-timeout", type=float, default=240, help="overall wall-clock timeout in seconds")
    parser.add_argument("--emulator-timeout", type=int, default=180, help="each emulator time limit in seconds")
    args = parser.parse_args()
    if args.host_timeout <= 0 or args.emulator_timeout <= 0:
        parser.error("timeouts must be greater than zero")

    try:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        (args.out_dir / "result.txt").write_text("RUNNING\n", encoding="utf-8")
        summary = run_with_timeout(lambda: run_test(args), args.host_timeout, label="HTTP integration")
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
