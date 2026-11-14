#!/usr/bin/env python3
"""Run native encrypted CLU UDP tests between two isolated esp-emu guests."""

import argparse
import hashlib
import json
import re
import secrets
import select
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.padding import PKCS7

from ..boot.run_boot import FAILURE_PATTERNS as BOOT_FAILURE_PATTERNS


CFG_OFFSET = 0x12000
CFG_SIZE = 0x3000
MASTER_KEY = bytes.fromhex("85de521e9c65d0de7e5845d35e53d09b")
FACTORY_IV = bytes.fromhex("b9afe387f919a3d3a0d79ade8e11a116")
BROADCAST_IV = bytes.fromhex("07060504030201000402090304010507")
FACTORY_PIN = b"ABCD1234"
FACTORY_DEVICE_KEY = bytes(a ^ b for a, b in zip(FACTORY_IV, FACTORY_PIN + FACTORY_PIN[::-1]))
# Session 00000001 under this test key decrypts to padding-valid garbage under the broadcast key.
KEY_SELECTION_KEY = bytes(range(16))
KEY_SELECTION_IV = bytes(range(16, 32))
KEY_SELECTION_SCRIPT = 'return "key-selection-ok" -- 103'
FAILURE_PATTERNS = BOOT_FAILURE_PATTERNS + (
    re.compile(r"bad UDP request|bad UDP sequence|bad UDP reply|missing timeout error|UDP test:"),
)


def encrypt(data: bytes, key: bytes, iv: bytes) -> bytes:
    padder = PKCS7(128).padder()
    padded = padder.update(data) + padder.finalize()
    encryptor = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
    return encryptor.update(padded) + encryptor.finalize()


def decrypt(data: bytes, key: bytes, iv: bytes) -> bytes:
    if not data or len(data) % 16:
        raise ValueError("AES-CBC ciphertext must be a non-empty multiple of 16 bytes")
    decryptor = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
    padded = decryptor.update(data) + decryptor.finalize()
    unpadder = PKCS7(128).unpadder()
    return unpadder.update(padded) + unpadder.finalize()


def response_session(text: str) -> str | None:
    fields = text.split(":", 3)
    if len(fields) >= 3 and fields[0] == "resp" and re.fullmatch(r"[0-9a-f]{8}", fields[2]):
        return fields[2]
    return None


def validate_packet_records(records: list[dict]) -> None:
    """Require six matched encrypted CLU exchanges and one intentionally dropped request."""
    by_direction = {
        direction: [row for row in records if row["direction"] == direction]
        for direction in ("request", "response", "dropped")
    }
    if tuple(len(by_direction[key]) for key in ("request", "response", "dropped")) != (6, 6, 1):
        raise ValueError("expected 6 forwarded requests, 6 responses, and 1 dropped request")

    request_parts = [row["plaintext"].split(":", 3) for row in by_direction["request"]]
    response_parts = [row["plaintext"].split(":", 3) for row in by_direction["response"]]
    if any(len(fields) < 3 or fields[0] != "req" for fields in request_parts):
        raise ValueError("forwarded packet did not decrypt to a CLU request")
    if any(len(fields) < 3 or fields[0] != "resp" for fields in response_parts):
        raise ValueError("forwarded packet did not decrypt to a CLU response")
    if any(request[2] != response[2] for request, response in zip(request_parts, response_parts)):
        raise ValueError("CLU request and response session IDs do not match")
    dropped = by_direction["dropped"][0]["plaintext"].split(":", 3)
    if len(dropped) < 3 or dropped[0] != "req":
        raise ValueError("the intentionally dropped packet was not a CLU request")


def run_with_timeout(callback, timeout: float, label: str = "UDP integration"):
    """Apply an independent wall-clock limit, unwinding runner cleanup on expiry (Unix)."""
    def expired(signum, frame):
        raise TimeoutError(f"{label} exceeded host timeout of {timeout:g}s")

    previous_handler = signal.signal(signal.SIGALRM, expired)
    signal.setitimer(signal.ITIMER_REAL, timeout)
    try:
        return callback()
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, previous_handler)


class Device:
    def __init__(
        self,
        name: str,
        udp_port: int,
        cfg_path: Path,
        firmware_path: Path,
        elf_path: Path,
        emulator: str,
        out_dir: Path,
        emulator_timeout: int,
        tcp_port: int | None = None,
        psram_size: str = "8M",
    ):
        self.name = name
        self.udp_port = udp_port
        self.tcp_port = tcp_port
        self.session = 0
        self.key = b""
        self.iv = b""
        self.pin_key = b""
        self.pin_iv = b""
        self.log_path = out_dir / f"{name}.log"
        self.log = None
        self.process = None
        self.sock = None

        image = bytearray(firmware_path.read_bytes())
        cfg = cfg_path.read_bytes()
        if len(cfg) != CFG_SIZE:
            raise ValueError(f"{cfg_path}: expected {CFG_SIZE:#x}-byte cfg partition, got {len(cfg):#x}")
        if len(image) < CFG_OFFSET + CFG_SIZE:
            image.extend(b"\xff" * (CFG_OFFSET + CFG_SIZE - len(image)))
        image[CFG_OFFSET : CFG_OFFSET + CFG_SIZE] = cfg
        flash_path = out_dir / f"flash-{name}.bin"
        flash_path.write_bytes(image)

        network = f"user,hostfwd=udp:127.0.0.1:{udp_port}-:1234"
        if tcp_port is not None:
            network += f",hostfwd=tcp:127.0.0.1:{tcp_port}-:80"
        command = [
            emulator,
            "--chip",
            "esp32s3",
            "--firmware",
            str(flash_path),
            "--elf",
            str(elf_path),
            "--psram-size",
            psram_size,
            "--net",
            network,
            "--timeout",
            f"{emulator_timeout}s",
        ]
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sock.connect(("127.0.0.1", udp_port))
            self.sock.settimeout(0.25)
            self.log = self.log_path.open("w", encoding="utf-8")
            self.process = subprocess.Popen(command, stdout=self.log, stderr=subprocess.STDOUT)
        except BaseException:
            self.close()
            raise

    def wait_for(self, marker: str, timeout: float = 60) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if marker in self.log_path.read_text(encoding="utf-8", errors="replace"):
                return
            if self.process.poll() is not None:
                raise RuntimeError(f"{self.name} emulator exited with {self.process.returncode}; see {self.log_path}")
            time.sleep(0.1)
        raise TimeoutError(f"{self.name}: timed out waiting for {marker!r}; see {self.log_path}")

    def _receive(self, timeout: float, key: bytes, iv: bytes) -> bytes:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                packet = self.sock.recv(4096)
            except socket.timeout:
                continue
            try:
                return decrypt(packet, key, iv)
            except ValueError:
                continue
        raise TimeoutError(f"{self.name}: UDP response timed out after {timeout:g}s")

    def discover(self, timeout: float = 15) -> None:
        reply_iv = secrets.token_bytes(16)
        packet = bytes(32) + b":" + reply_iv + b":req_discovery_clu:"
        encrypted = encrypt(packet, MASTER_KEY, BROADCAST_IV)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.sock.send(encrypted)
            try:
                reply = self._receive(min(2, max(0.1, deadline - time.monotonic())), MASTER_KEY, reply_iv)
            except TimeoutError:
                continue
            if b":resp_discovery_clu:" not in reply or len(reply) < 49:
                continue
            # PIN key + temp IV: accepted for req_set_key while the setup window is open, also
            # after a project key is set, so a key change can be retried whatever the CLU holds.
            self.pin_key, self.pin_iv = FACTORY_DEVICE_KEY, reply[33:49]
            self.key, self.iv = self.pin_key, self.pin_iv
            return
        raise TimeoutError(f"{self.name}: CLU discovery timed out")

    def set_project_key(self, key: bytes, iv: bytes, retries: int = 1, timeout: float = 15) -> None:
        """Set the project key under the PIN key (idempotent, so a lost reply is retried)."""
        message = bytes(32) + b":" + iv + b":req_set_key:" + key
        packet = encrypt(message, self.pin_key, self.pin_iv)
        for _ in range(retries + 1):
            self.sock.send(packet)
            deadline = time.monotonic() + timeout
            while (remaining := deadline - time.monotonic()) > 0:
                try:
                    response = self._receive(remaining, key, iv)
                except TimeoutError:
                    break
                # Late discovery reply copies (sent 3x) can decrypt with valid padding under the
                # new key: anything that is not a CLU response is skipped, not reported.
                if not response.startswith(b"resp"):
                    continue
                if response != b"resp:OK":
                    raise RuntimeError(f"{self.name}: project key setup failed: {response!r}")
                self.key, self.iv = key, iv
                return
        raise TimeoutError(f"{self.name}: project key setup timed out")

    def call(self, script: str, retries: int = 0, timeout: float = 15) -> str:
        """Run a Lua request, optionally retrying only a caller-designated idempotent script."""
        for attempt in range(retries + 1):
            self.session += 1
            session = f"{self.session:08x}"
            message = f"req:192.168.4.1:{session}:{script}".encode()
            self.sock.send(encrypt(message, self.key, self.iv))
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                try:
                    packet = self.sock.recv(4096)
                except socket.timeout:
                    continue
                try:
                    text = decrypt(packet, self.key, self.iv).decode()
                except (UnicodeDecodeError, ValueError):
                    continue
                if response_session(text) != session:
                    continue
                fields = text.split(":", 3)
                return fields[3] if len(fields) == 4 else ""
            if attempt == retries:
                raise TimeoutError(f"{self.name}: Lua request session {session} timed out")
        raise AssertionError("unreachable")

    def upload(self, script: str) -> None:
        # All retryable Lua statements below are idempotent assignments or file read/write operations.
        if self.call('upload_parts = {}; return "ok"', retries=1) != "ok":
            raise RuntimeError(f"{self.name}: could not initialize Lua upload buffer")
        for index, offset in enumerate(range(0, len(script), 600), 1):
            chunk = json.dumps(script[offset : offset + 600])
            if self.call(f'upload_parts[{index}] = {chunk}; return "ok"', retries=1) != "ok":
                raise RuntimeError(f"{self.name}: could not upload Lua chunk {index}")
        length = self.call(
            'local s=table.concat(upload_parts); local f=assert(io.open("/fs/a/MAIN.LUA","wb")); '
            'assert(f:write(s)); assert(f:close()); return #s',
            retries=1,
        )
        if length != str(len(script)):
            raise RuntimeError(f"{self.name}: stored Lua length {length!r} did not match {len(script)}")
        verified = self.call(
            'local f=assert(io.open("/fs/a/MAIN.LUA","rb")); local s=f:read("a"); f:close(); '
            'return s==table.concat(upload_parts)',
            retries=1,
        )
        if verified != "true":
            raise RuntimeError(f"{self.name}: stored MAIN.LUA read-back verification failed")
        if self.call('assert(loadfile("/fs/a/MAIN.LUA"))(); return "loaded"') != "loaded":
            raise RuntimeError(f"{self.name}: MAIN.LUA did not load")

    def close(self) -> None:
        if self.sock is not None:
            self.sock.close()
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if self.log is not None:
            self.log.close()


class Relay:
    """Forward encrypted UDP bytes unchanged between emulator host-forward ports."""

    def __init__(self, key: bytes, iv: bytes, bind_port: int = 1234):
        self.key = key
        self.iv = iv
        self.target_port = None
        self.records = []
        self.errors = []
        self.flows = {}
        self.stop = threading.Event()
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.listener.bind(("127.0.0.1", bind_port))
        self.thread = threading.Thread(target=self._run, name="clu-udp-relay", daemon=True)
        self.thread.start()

    def _record(self, direction: str, data: bytes) -> None:
        plaintext = decrypt(data, self.key, self.iv).decode("utf-8")
        self.records.append(
            {
                "direction": direction,
                "bytes": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "plaintext": plaintext,
            }
        )

    def _run(self) -> None:
        try:
            while not self.stop.is_set():
                readable, _, _ = select.select([self.listener, *self.flows], [], [], 0.1)
                for sock in readable:
                    if sock is self.listener:
                        data, sender = self.listener.recvfrom(4096)
                        if self.target_port is None:
                            self._record("dropped", data)
                            continue
                        self._record("request", data)
                        upstream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                        upstream.connect(("127.0.0.1", self.target_port))
                        self.flows[upstream] = sender
                        upstream.send(data)
                    else:
                        data = sock.recv(4096)
                        self._record("response", data)
                        self.listener.sendto(data, self.flows.pop(sock))
                        sock.close()
        except OSError as exc:
            if not self.stop.is_set():
                self.errors.append(repr(exc))
        except Exception as exc:
            self.errors.append(repr(exc))

    def close(self) -> None:
        self.stop.set()
        self.thread.join(timeout=3)
        for sock in self.flows:
            sock.close()
        self.listener.close()
        if self.thread.is_alive():
            raise RuntimeError("UDP relay did not stop")


def check_key_selection(device: Device) -> None:
    """Send the padding-collision vector as the first Lua session under KEY_SELECTION_KEY.

    Not retried: a retry uses another session, whose ciphertext does not collide, and would hide
    a firmware that stops at the first key with valid padding.
    """
    if device.session != 0:
        raise RuntimeError(f"{device.name}: key selection vector needs Lua session 00000001")
    try:
        result = device.call(KEY_SELECTION_SCRIPT)
    except TimeoutError as exc:
        raise RuntimeError(
            f"{device.name}: padding collision selected the wrong UDP key (request dropped)"
        ) from exc
    if result != "key-selection-ok":
        raise RuntimeError(f"{device.name}: padding collision selected the wrong UDP key: {result!r}")


def check_logs(devices: list[Device], nonce: str) -> None:
    for device in devices:
        if device.process.poll() is not None:
            raise RuntimeError(f"{device.name}: emulator exited early with {device.process.returncode}")
        output = device.log_path.read_text(encoding="utf-8", errors="replace")
        for pattern in FAILURE_PATTERNS:
            match = pattern.search(output)
            if match:
                raise RuntimeError(f"{device.name}: emulator log contains failure signature {match.group(0)!r}")
        expected = [f"UDP_READY {device.name} {nonce}"]
        if device.name == "A":
            expected.append(f"UDP_TIMEOUT_OK A {nonce}")
        for marker in expected:
            if marker not in output:
                raise RuntimeError(f"{device.name}: missing Lua marker {marker!r}")


def run_test(args: argparse.Namespace) -> str:
    for path in (args.firmware, args.elf, args.cfg_a, args.cfg_b):
        if not path.is_file():
            raise FileNotFoundError(f"required file does not exist: {path}")
    args.out_dir.mkdir(parents=True, exist_ok=True)

    devices = []
    relay = None
    nonce = secrets.token_hex(8)
    project_key = secrets.token_bytes(16)
    project_iv = secrets.token_bytes(16)
    template = (Path(__file__).with_name("udp_peer.lua")).read_text(encoding="utf-8")
    try:
        for name, port, cfg in (("A", 19201, args.cfg_a), ("B", 19202, args.cfg_b)):
            devices.append(
                Device(
                    name,
                    port,
                    cfg,
                    args.firmware,
                    args.elf,
                    args.emulator,
                    args.out_dir,
                    args.emulator_timeout,
                    psram_size=getattr(args, "psram_size", "8M"),
                )
            )
        for device in devices:
            device.wait_for("main: WiFi IP 192.168.4.2")
            device.wait_for("no /fs/a/MAIN.LUA, idle")
            device.discover()
            device.set_project_key(KEY_SELECTION_KEY, KEY_SELECTION_IV)
            check_key_selection(device)
            device.discover()
            device.set_project_key(project_key, project_iv)
            peer = "B" if device.name == "A" else "A"
            script = template.replace("@NAME@", device.name).replace("@PEER@", peer).replace("@NONCE@", nonce)
            (args.out_dir / f"MAIN-{device.name}.LUA").write_text(script, encoding="utf-8")
            device.upload(script)
            device.wait_for(f"UDP_READY {device.name} {nonce}")

        relay = Relay(project_key, project_iv)
        for source, target in ((devices[0], devices[1]), (devices[1], devices[0])):
            relay.target_port = target.udp_port
            if source.call("return udp_start(false)", retries=1) != "started":
                raise RuntimeError(f"{source.name}: failed to start native UDP exchange")
            source.wait_for(f"UDP_DONE {source.name} {nonce}", timeout=45)
            if source.call("return udp_completed", retries=1) != "3":
                raise RuntimeError(f"{source.name}: not all three CLU UDP replies completed")
            if target.call("return udp_received", retries=1) != "3":
                raise RuntimeError(f"{target.name}: did not receive all three CLU UDP requests")
            for sequence in range(1, 4):
                source.wait_for(f"UDP_REPLY_OK {source.name} {sequence} {nonce}")
                target.wait_for(f"UDP_RECEIVED {target.name} {nonce}:{source.name}:ping:{sequence}")

        relay.target_port = None
        if devices[0].call("return udp_start(true)", retries=1) != "started":
            raise RuntimeError("A: failed to start dropped-packet timeout check")
        devices[0].wait_for(f"UDP_TIMEOUT_OK A {nonce}", timeout=30)
        if devices[1].call("return udp_received", retries=1) != "3":
            raise RuntimeError("B received a packet that should have been dropped")
        if relay.errors:
            raise RuntimeError(f"UDP relay failed: {relay.errors}")
        validate_packet_records(relay.records)
        check_logs(devices, nonce)
        summary = (
            "PASS: native encrypted CLU UDP A -> B 3/3, B -> A 3/3; exact nonce-tagged Lua "
            f"requests/replies, wrong-key padding collision and dropped-packet timeout verified. nonce={nonce}"
        )
        return summary
    finally:
        try:
            if relay is not None:
                try:
                    relay.close()
                finally:
                    (args.out_dir / "packets.json").write_text(
                        json.dumps(relay.records, indent=2) + "\n", encoding="utf-8"
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
        (args.out_dir / "packets.json").write_text("[]\n", encoding="utf-8")
        summary = run_with_timeout(lambda: run_test(args), args.host_timeout)
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
