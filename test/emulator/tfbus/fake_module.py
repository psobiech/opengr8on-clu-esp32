"""Emulated TF-Bus DIMMER_MOSFET_DIN on the far end of esp-emu's UART1 TCP bridge.

esp-emu (`--uart1-tcp HOST:PORT`) serves UART1 as a TCP server; UART1 TX is discarded until a
client connects. ModuleLink connects as that client, splits the CLU's RS485 byte stream into
frames and answers them as one module would on the wire. Frame formats follow
components/clu_proto/tfbus_frame.c (CLU -> module) and tfbus_vmod.c (module -> CLU).
"""

import json
import socket
import threading
import time
from pathlib import Path


START_BYTE = 0xAA
BROADCAST_ADDRESS = 0xFF
CLU_ADDRESS = 0x00
MAX_FRAME_LENGTH = 32

FRAME_ADDR_ASSIGN = 0x02
FRAME_STATUS_POLL = 0x04
FRAME_RESET = 0x07
FRAME_DISCOVER = 0x08
FRAME_SEEK = 0x2A
FRAME_MODULE_CMD = 0xF0
FRAME_VERSION_QUERY = 0xF9

# CLU -> module frame lengths by type (tfbus_frame.c typed_len); MODULE_CMD carries its own length.
TYPED_FRAME_LENGTHS = {
    0x02: 9,
    0x04: 4,
    0x07: 4,
    0x09: 5,
    0x0A: 4,
    0x0B: 4,
    0x18: 7,
    0x2A: 4,
    0x31: 26,
    0xF9: 4,
    0xFB: 25,
    0xFD: 7,
    0xFE: 7,
}

DIMMER_HW_TYPE = 0x1A
DIMMER_FW_TYPE = 0x02
DIMMER_FW_API_VERSION = 0x02
# VERSION_QUERY reply: fwApiVer verA verB hwVer -> fwVer "2.0.2", hwVer 1.
DIMMER_VERSION = bytes([DIMMER_FW_API_VERSION, 0, 2, 1])
RECORD_SET_RAMP_TIME = 0x06
RECORD_SET_VALUE = 0x0C


class ModuleLink:
    """Connect to esp-emu's UART1 TCP server and answer the CLU's frames as @p module."""

    def __init__(self, module: "DimmerModule", port: int, frames_path: Path, connect_timeout: float = 60):
        self.module = module
        self.port = port
        self.connect_timeout = connect_timeout
        self.condition = threading.Condition()
        self.connected = False
        self.error: BaseException | None = None
        self.stopping = threading.Event()
        self.connection: socket.socket | None = None
        self.started_at = time.monotonic()
        self.frames_file = frames_path.open("w", encoding="utf-8")
        self.thread = threading.Thread(target=self._run, name="tfbus-module", daemon=True)
        self.thread.start()

    def wait_until(self, predicate, timeout: float, description: str) -> None:
        """Wait until @p predicate (called with the module state locked) holds."""
        with self.condition:
            satisfied = self.condition.wait_for(lambda: self.error is not None or predicate(), timeout)
            if self.error is not None:
                raise RuntimeError(f"TF-Bus module link failed: {self.error}") from self.error
            if not satisfied:
                raise TimeoutError(f"TF-Bus module: timed out waiting for {description}")

    def command_count(self) -> int:
        with self.condition:
            return len(self.module.commands)

    def commands_since(self, first_index: int) -> list[tuple[int, int, bytes]]:
        with self.condition:
            return list(self.module.commands[first_index:])

    def address(self) -> int:
        with self.condition:
            return self.module.address

    def set_level(self, level: int) -> None:
        """Change the output level as the module itself would (local input, own ramp)."""
        with self.condition:
            self.module.level = level

    def set_powered(self, powered: bool) -> None:
        with self.condition:
            if powered:
                self.module.power_on()
            else:
                self.module.power_off()

    def close(self) -> None:
        self.stopping.set()
        if self.connection is not None:
            try:
                self.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.thread.join(timeout=5)
        if self.connection is not None:
            self.connection.close()
        self.frames_file.close()

    def _run(self) -> None:
        try:
            self.connection = self._connect()
            if self.connection is None:
                return
            with self.condition:
                self.connected = True
                self.condition.notify_all()
            self._serve(self.connection)
        except BaseException as exc:  # reported to the waiting test thread
            with self.condition:
                if not self.stopping.is_set():
                    self.error = exc
                self.condition.notify_all()

    def _connect(self) -> socket.socket | None:
        """Retry until esp-emu listens: the CLU's startup RESET / DISCOVER must reach the module."""
        deadline = time.monotonic() + self.connect_timeout
        while not self.stopping.is_set():
            try:
                connection = socket.create_connection(("127.0.0.1", self.port), timeout=1)
            except OSError:
                if time.monotonic() >= deadline:
                    raise TimeoutError(f"esp-emu UART1 bridge on port {self.port} did not accept a connection")
                time.sleep(0.05)
                continue
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            connection.settimeout(0.2)
            return connection
        return None

    def _serve(self, connection: socket.socket) -> None:
        parser = FrameParser()
        while not self.stopping.is_set():
            try:
                received = connection.recv(256)
            except socket.timeout:
                continue
            if not received:
                if self.stopping.is_set():
                    return
                raise ConnectionError("esp-emu closed the UART1 bridge")
            for frame in parser.feed(received):
                with self.condition:
                    reply = self.module.handle(frame)
                    self._record("clu", frame)
                    if reply is not None:
                        self._record("module", reply)
                    self.condition.notify_all()
                if reply is not None:
                    connection.sendall(reply)

    def _record(self, sender: str, frame: bytes) -> None:
        elapsed_seconds = round(time.monotonic() - self.started_at, 6)
        self.frames_file.write(json.dumps({"seconds": elapsed_seconds, "from": sender, "frame": frame.hex(" ")}) + "\n")
        self.frames_file.flush()


class DimmerModule:
    """Module-side state machine of one DIMMER_MOSFET_DIN (hwType 0x1a, docs/TFBUS.md)."""

    def __init__(self, serial_number: int, level: int = 0, inputs: int = 0):
        self.serial_number = serial_number
        self.level = level  # 0-1000
        self.inputs = inputs
        self.address = 0
        self.powered = True
        self.commands: list[tuple[int, int, bytes]] = []  # MODULE_CMD records: (type, meta, data)
        self.frame_counts: dict[int, int] = {}

    def power_off(self) -> None:
        self.powered = False

    def power_on(self) -> None:
        """Back on the bus unaddressed; the output level is restored, as the module keeps it."""
        self.powered = True
        self.address = 0

    def handle(self, frame: bytes) -> bytes | None:
        """Answer one valid CLU -> module frame; None = stay silent."""
        destination, frame_type, payload = frame[1], frame[2], frame[3:-1]
        self.frame_counts[frame_type] = self.frame_counts.get(frame_type, 0) + 1
        if not self.powered:
            return None
        if destination == BROADCAST_ADDRESS:
            return self._broadcast(frame_type, payload)
        if not self.address or destination != self.address:
            return None
        if frame_type == FRAME_RESET:
            self.address = 0
            return None
        if frame_type == FRAME_STATUS_POLL:
            return build_reply(self.status_payload())
        if frame_type == FRAME_VERSION_QUERY:
            return build_reply(DIMMER_VERSION)
        if frame_type == FRAME_MODULE_CMD:
            self._apply_module_command(payload)
        return build_reply(b"")

    def status_payload(self) -> bytes:
        """STATUS_POLL reply: 2 records, level (01 02 10 [level LE u16]) and inputs (00 01 01 [inputs])."""
        return bytes([2, 0x01, 0x02, 0x10, self.level & 0xFF, self.level >> 8, 0x00, 0x01, 0x01, self.inputs])

    def _broadcast(self, frame_type: int, payload: bytes) -> bytes | None:
        if frame_type == FRAME_RESET:
            self.address = 0
            return None
        if frame_type == FRAME_SEEK:
            return build_reply(b"")
        if frame_type == FRAME_DISCOVER:
            if self.address:
                return None
            identity = bytes([DIMMER_HW_TYPE, DIMMER_FW_TYPE, DIMMER_FW_API_VERSION])
            return build_reply(self.serial_number.to_bytes(4, "little") + identity)
        if frame_type == FRAME_ADDR_ASSIGN and len(payload) >= 5:
            if int.from_bytes(payload[:4], "little") != self.serial_number:
                return None
            self.address = payload[4]
            return build_reply(b"")
        return None

    def _apply_module_command(self, payload: bytes) -> None:
        """Payload: plen count {type len meta data}... (tfbus_module_cmd)."""
        for record_type, meta, data in parse_module_command(payload):
            self.commands.append((record_type, meta, data))
            if record_type == RECORD_SET_VALUE and meta == 0 and len(data) == 3 and data[0] == 0:
                self.level = data[1] | data[2] << 8


class FrameParser:
    """Split the CLU -> module byte stream into checksum-valid frames (tfbus_rx_push)."""

    def __init__(self):
        self.buffer = bytearray()

    def feed(self, data: bytes) -> list[bytes]:
        self.buffer.extend(data)
        frames = []
        while True:
            start = self.buffer.find(START_BYTE)
            if start < 0:
                self.buffer.clear()
                return frames
            del self.buffer[:start]
            if len(self.buffer) < 4:
                return frames
            frame_length = expected_frame_length(self.buffer)
            if frame_length is None:
                frame_length = zero_checksum_prefix_length(self.buffer)
                if frame_length is None:
                    if len(self.buffer) < MAX_FRAME_LENGTH:
                        return frames
                    del self.buffer[0]
                    continue
            if frame_length > MAX_FRAME_LENGTH:
                del self.buffer[0]
                continue
            if len(self.buffer) < frame_length:
                return frames
            frame = bytes(self.buffer[:frame_length])
            if xor_checksum(frame) != 0:
                del self.buffer[0]
                continue
            frames.append(frame)
            del self.buffer[:frame_length]


def parse_module_command(payload: bytes) -> list[tuple[int, int, bytes]]:
    """MODULE_CMD payload -> [(type, meta, data)]; a malformed payload raises ValueError."""
    if len(payload) < 2 or payload[0] != len(payload):
        raise ValueError(f"bad MODULE_CMD payload {payload.hex(' ')}")
    records = []
    offset = 2
    for _ in range(payload[1]):
        if offset + 3 > len(payload):
            raise ValueError(f"truncated MODULE_CMD record in {payload.hex(' ')}")
        record_type, record_length, meta = payload[offset], payload[offset + 1], payload[offset + 2]
        offset += 3
        if offset + record_length > len(payload):
            raise ValueError(f"truncated MODULE_CMD record data in {payload.hex(' ')}")
        records.append((record_type, meta, bytes(payload[offset : offset + record_length])))
        offset += record_length
    if offset != len(payload):
        raise ValueError(f"trailing bytes in MODULE_CMD payload {payload.hex(' ')}")
    return records


def expected_frame_length(buffer: bytearray) -> int | None:
    frame_type = buffer[2]
    if frame_type == FRAME_MODULE_CMD:
        return 4 + buffer[3]
    return TYPED_FRAME_LENGTHS.get(frame_type)


def zero_checksum_prefix_length(buffer: bytearray) -> int | None:
    """Unknown frame type: the shortest prefix whose last byte is the XOR of the others."""
    for frame_length in range(4, min(len(buffer), MAX_FRAME_LENGTH) + 1):
        if xor_checksum(buffer[:frame_length]) == 0:
            return frame_length
    return None


def build_reply(payload: bytes) -> bytes:
    """Module -> CLU frame: AA 00 len payload XOR, len = payload + 1."""
    frame = bytes([START_BYTE, CLU_ADDRESS, len(payload) + 1]) + payload
    return frame + bytes([xor_checksum(frame)])


def build_frame(destination: int, frame_type: int, payload: bytes = b"") -> bytes:
    """CLU -> module frame: AA dest type payload XOR (tfbus_build), for tests."""
    frame = bytes([START_BYTE, destination, frame_type]) + payload
    return frame + bytes([xor_checksum(frame)])


def xor_checksum(data: bytes | bytearray) -> int:
    checksum = 0
    for byte in data:
        checksum ^= byte
    return checksum
