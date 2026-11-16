#!/usr/bin/env python3
"""Host client for the secure CLU channel (Noise_IK_25519_AESGCM_SHA256 over UDP, see SECURE.md).

    clu_sec.py fingerprint                 this host's key fingerprint (key created on first use)
    clu_sec.py pubkey <ip>                 probe a CLU: serial, public key, fingerprint
    clu_sec.py pair <ip>                   pair with a CLU (its setup window must be open)
    clu_sec.py exec <ip> <lua>             run a Lua expression on a paired CLU
    clu_sec.py forget <serial>             drop a CLU from this host's paired list

Needs: pip install noiseprotocol cryptography
Files: ~/.config/opengr8on/host.key (X25519 private key), ~/.config/opengr8on/clus.json (pinned CLUs).
"""
import argparse
import hashlib
import json
import os
import secrets
import socket
import struct
import sys
import time
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from noise.connection import Keypair, NoiseConnection

NAME = b"Noise_IK_25519_AESGCM_SHA256"
PROLOGUE = b"opengr8on-sec-v1"
VERSION = 2
T_HS_INIT, T_HS_RESP, T_DATA, T_PROBE, T_PROBE_RESP = 1, 2, 4, 8, 9
PROBE_LEN = 48

CONF = Path(os.environ.get("OPENGR8ON_CONF", Path.home() / ".config" / "opengr8on"))
KEY_FILE = CONF / "host.key"
PINS_FILE = CONF / "clus.json"


def fingerprint(pub: bytes) -> str:
    return hashlib.sha256(pub).digest()[:8].hex()


def host_key() -> bytes:
    if KEY_FILE.exists():
        return KEY_FILE.read_bytes()
    CONF.mkdir(parents=True, exist_ok=True)
    key = secrets.token_bytes(32)
    file_descriptor = os.open(KEY_FILE, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(file_descriptor, "wb") as key_file:
        key_file.write(key)
    return key


def host_pub(priv: bytes) -> bytes:
    connection = NoiseConnection.from_name(NAME)
    connection.set_keypair_from_private_bytes(Keypair.STATIC, priv)
    return connection.noise_protocol.keypairs["s"].public_bytes


def load_pins() -> dict:
    return json.loads(PINS_FILE.read_text()) if PINS_FILE.exists() else {}


def save_pins(pins: dict) -> None:
    CONF.mkdir(parents=True, exist_ok=True)
    PINS_FILE.write_text(json.dumps(pins, indent=2) + "\n")


APP_REQ, APP_RESP, APP_ACK = 1, 2, 3


class Channel:
    last_timestamp_ms = 0  # keeps HS_INIT timestamps strictly increasing

    def __init__(self, peer_ip: str, port: int, timeout: float):
        self.addr = (peer_ip, port)
        self.timeout = timeout
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(timeout)
        self.sock.connect(self.addr)

    def recv(self, want_type: int) -> bytes:
        while True:
            data = self.sock.recv(2048)
            if data and data[0] == want_type:
                return data

    def probe(self) -> tuple[int, bytes]:
        nonce = secrets.randbits(32)
        pkt = bytes([T_PROBE]) + struct.pack("<I", nonce) + bytes(PROBE_LEN - 5)
        for _ in range(3):
            self.sock.send(pkt)
            try:
                answer = self.recv(T_PROBE_RESP)
            except socket.timeout:
                continue
            if len(answer) >= 42 and struct.unpack_from("<I", answer, 1)[0] == nonce and answer[5] == VERSION:
                return struct.unpack_from("<I", answer, 6)[0], answer[10:42]
        raise SystemExit(f"{self.addr[0]}: no answer on udp/{self.addr[1]} (secure channel off?)")

    def handshake(self, priv: bytes, remote_pub: bytes) -> int:
        connection = NoiseConnection.from_name(NAME)
        connection.set_prologue(PROLOGUE)
        connection.set_as_initiator()
        connection.set_keypair_from_private_bytes(Keypair.STATIC, priv)
        connection.set_keypair_from_public_bytes(Keypair.REMOTE_STATIC, remote_pub)
        connection.start_handshake()
        self.local_idx = secrets.randbits(32) or 1
        timestamp_ms = max(time.time_ns() // 1_000_000, Channel.last_timestamp_ms + 1)
        Channel.last_timestamp_ms = timestamp_ms
        msg1 = connection.write_message(struct.pack("<BBIQ", VERSION, ord("h"), 0, timestamp_ms))
        self.sock.send(bytes([T_HS_INIT]) + struct.pack("<I", self.local_idx) + msg1)
        try:
            answer = self.recv(T_HS_RESP)
        except socket.timeout:
            raise SystemExit(f"{self.addr[0]}: handshake refused (not paired, or the CLU's setup window is closed)")
        sender, receiver = struct.unpack_from("<II", answer, 1)
        if receiver != self.local_idx:
            raise SystemExit("handshake: unexpected session index")
        payload = connection.read_message(answer[9:])
        if len(payload) != 14:
            raise SystemExit("handshake: malformed payload")
        version, _kind, serial, _timestamp_ms = struct.unpack("<BBIQ", payload)
        if version != VERSION:
            raise SystemExit(f"unsupported protocol version {version}")
        self.remote_idx = sender
        self.k_send = connection.noise_protocol.cipher_state_encrypt.k
        self.k_recv = connection.noise_protocol.cipher_state_decrypt.k
        self.n_send = 0
        return serial

    @staticmethod
    def nonce(counter: int) -> bytes:
        return bytes(4) + struct.pack(">Q", counter)

    def send(self, plain: bytes) -> None:
        header = bytes([T_DATA]) + struct.pack("<IQ", self.remote_idx, self.n_send)
        self.sock.send(header + AESGCM(self.k_send).encrypt(self.nonce(self.n_send), plain, header))
        self.n_send += 1

    def recv_data(self) -> bytes:
        while True:
            packet = self.recv(T_DATA)
            index, counter = struct.unpack_from("<IQ", packet, 1)
            if index != self.local_idx:
                continue
            try:
                return AESGCM(self.k_recv).decrypt(self.nonce(counter), packet[13:], packet[:13])
            except Exception:
                continue


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=1235)
    parser.add_argument("--timeout", type=float, default=2.0)
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("fingerprint")
    for name in ("pubkey", "pair"):
        sub.add_parser(name).add_argument("address")
    exec_parser = sub.add_parser("exec")
    exec_parser.add_argument("address")
    exec_parser.add_argument("lua")
    sub.add_parser("forget").add_argument("serial", type=int)
    args = parser.parse_args()

    priv = host_key()
    if args.cmd == "fingerprint":
        print(fingerprint(host_pub(priv)))
        return
    if args.cmd == "forget":
        pins = load_pins()
        print("removed" if pins.pop(str(args.serial), None) else "not paired")
        save_pins(pins)
        return

    channel = Channel(args.address, args.port, args.timeout)
    serial, pub = channel.probe()
    pins = load_pins()
    pinned = pins.get(str(serial))
    if args.cmd == "pubkey":
        state = "paired" if pinned == pub.hex() else "KEY MISMATCH" if pinned else "not paired"
        print(f"sn {serial} key {pub.hex()} fingerprint {fingerprint(pub)} ({state})")
        return
    if pinned and pinned != pub.hex():
        raise SystemExit(f"CLU {serial} at {args.address} presents key {fingerprint(pub)}, pinned "
                         f"{fingerprint(bytes.fromhex(pinned))}: refusing (clu_sec.py forget {serial} to re-pair)")
    if args.cmd == "exec" and not pinned:
        raise SystemExit(f"CLU {serial} not paired: open its setup window and run: clu_sec.py pair {args.address}")
    handshake_serial = channel.handshake(priv, pub)
    if handshake_serial != serial:
        raise SystemExit(f"handshake serial {handshake_serial} != probed {serial}")
    if args.cmd == "pair":
        # The CLU pins this host on the first authentic request (proof the handshake completed).
        request(channel, args.address, "checkAlive()")
        pins[str(serial)] = pub.hex()
        save_pins(pins)
        print(f"paired with CLU {serial} (key {fingerprint(pub)}); this host is {fingerprint(host_pub(priv))}")
        return
    print(request(channel, args.address, args.lua))


def request(channel: Channel, peer_ip: str, lua: str) -> str:
    """Binary call: REQ(1) id(4 LE) script -> ACK(3) id / RESP(2) id status(1) value. REQ is resent until ACKed."""
    call_id = secrets.randbits(32)
    req = bytes([APP_REQ]) + struct.pack("<I", call_id) + lua.encode()
    deadline = time.monotonic() + channel.timeout
    next_tx, acked = 0.0, False
    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_tx:
            channel.send(req)
            next_tx = now + (2.0 if acked else 0.3)
        channel.sock.settimeout(max(0.05, min(next_tx, deadline) - time.monotonic()))
        try:
            msg = channel.recv_data()
        except socket.timeout:
            continue
        if len(msg) < 5 or struct.unpack_from("<I", msg, 1)[0] != call_id:
            continue
        if msg[0] == APP_ACK:
            acked = True
        elif msg[0] == APP_RESP and len(msg) >= 6:
            if msg[5] != 0:
                raise SystemExit("resp:ERROR")
            return msg[6:].decode(errors="replace")
    raise SystemExit("no reply")


if __name__ == "__main__":
    main()
