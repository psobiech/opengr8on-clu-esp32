import argparse
import json
import socket
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from .fake_module import (
    BROADCAST_ADDRESS,
    FRAME_ADDR_ASSIGN,
    FRAME_DISCOVER,
    FRAME_MODULE_CMD,
    FRAME_RESET,
    FRAME_SEEK,
    FRAME_STATUS_POLL,
    FRAME_VERSION_QUERY,
    DimmerModule,
    FrameParser,
    ModuleLink,
    build_frame,
    build_reply,
    parse_module_command,
)
from .run_tfbus import run_test, validate_set_value_commands, validate_tfbus_log


SERIAL_NUMBER = 330000101
ACK = bytes.fromhex("aa 00 01 ab")
DISCOVERY_REPLY_LENGTH = 11  # AA 00 08, serial LE u32, hwType fwType fwApiVer, XOR


def module_command(*records: tuple[int, int, bytes]) -> bytes:
    """MODULE_CMD payload as tfbus_module_cmd builds it: plen count {type len meta data}..."""
    body = b"".join(bytes([record_type, len(data), meta]) + data for record_type, meta, data in records)
    return bytes([2 + len(body), len(records)]) + body


def assign_frame(serial_number: int, address: int) -> bytes:
    return build_frame(BROADCAST_ADDRESS, FRAME_ADDR_ASSIGN, serial_number.to_bytes(4, "little") + bytes([address]))


class FrameParserTests(unittest.TestCase):
    def test_frames_split_across_reads_and_garbage_are_resynchronised(self):
        parser = FrameParser()
        assign = assign_frame(SERIAL_NUMBER, 1)
        poll = build_frame(1, FRAME_STATUS_POLL)
        command = build_frame(1, FRAME_MODULE_CMD, module_command((0x0C, 0, bytes([0, 0xF4, 0x01]))))
        stream = b"\x00\x13" + assign + poll + command
        frames = parser.feed(stream[:5]) + parser.feed(stream[5:13]) + parser.feed(stream[13:])
        self.assertEqual(frames, [assign, poll, command])

    def test_bad_checksum_is_dropped(self):
        poll = bytearray(build_frame(1, FRAME_STATUS_POLL))
        poll[-1] ^= 0xFF
        self.assertEqual(FrameParser().feed(bytes(poll) + build_frame(2, FRAME_STATUS_POLL)), [build_frame(2, FRAME_STATUS_POLL)])

    def test_unknown_type_uses_shortest_zero_checksum_prefix(self):
        frame = build_frame(1, 0x55, b"\x01\x02")
        self.assertEqual(FrameParser().feed(frame), [frame])


class DimmerModuleTests(unittest.TestCase):
    def addressed(self) -> DimmerModule:
        module = DimmerModule(SERIAL_NUMBER, level=0)
        module.handle(assign_frame(SERIAL_NUMBER, 3))
        return module

    def test_discovery_and_address_assignment(self):
        module = DimmerModule(SERIAL_NUMBER)
        self.assertEqual(module.handle(build_frame(BROADCAST_ADDRESS, FRAME_SEEK)), ACK)
        self.assertEqual(
            module.handle(build_frame(BROADCAST_ADDRESS, FRAME_DISCOVER)),
            build_reply(SERIAL_NUMBER.to_bytes(4, "little") + bytes([0x1A, 0x02, 0x02])),
        )
        self.assertIsNone(module.handle(assign_frame(SERIAL_NUMBER + 1, 3)))
        self.assertEqual(module.handle(assign_frame(SERIAL_NUMBER, 3)), ACK)
        self.assertEqual(module.address, 3)
        self.assertIsNone(module.handle(build_frame(BROADCAST_ADDRESS, FRAME_DISCOVER)))

    def test_status_poll_reports_level_and_inputs(self):
        module = self.addressed()
        module.level, module.inputs = 500, 1
        self.assertEqual(
            module.handle(build_frame(3, FRAME_STATUS_POLL)),
            build_reply(bytes.fromhex("02 01 02 10 f4 01 00 01 01 01")),
        )
        self.assertIsNone(module.handle(build_frame(4, FRAME_STATUS_POLL)))
        self.assertEqual(module.handle(build_frame(3, FRAME_VERSION_QUERY)), build_reply(bytes([2, 0, 2, 1])))

    def test_module_command_is_recorded_acked_and_sets_the_level(self):
        module = self.addressed()
        payload = module_command((0x06, 0, bytes([0, 0, 0, 0, 0])), (0x0C, 0, bytes([0, 0xF4, 0x01])))
        self.assertEqual(module.handle(build_frame(3, FRAME_MODULE_CMD, payload)), ACK)
        self.assertEqual(module.commands, [(0x06, 0, bytes(5)), (0x0C, 0, bytes([0, 0xF4, 0x01]))])
        self.assertEqual(module.level, 500)

    def test_malformed_module_command_raises(self):
        with self.assertRaises(ValueError):
            parse_module_command(bytes([9, 1, 0x0C, 3, 0]))

    def test_reset_and_power_cycle_drop_the_address_but_keep_the_level(self):
        module = self.addressed()
        module.level = 250
        self.assertIsNone(module.handle(build_frame(3, FRAME_RESET)))
        self.assertEqual(module.address, 0)
        module.handle(assign_frame(SERIAL_NUMBER, 3))
        module.power_off()
        self.assertIsNone(module.handle(build_frame(3, FRAME_STATUS_POLL)))
        self.assertIsNone(module.handle(build_frame(BROADCAST_ADDRESS, FRAME_SEEK)))
        module.power_on()
        self.assertEqual((module.address, module.level), (0, 250))
        self.assertIsNone(module.handle(build_frame(3, FRAME_STATUS_POLL)))
        self.assertEqual(module.handle(assign_frame(SERIAL_NUMBER, 3)), ACK)


class ModuleLinkTests(unittest.TestCase):
    def test_link_answers_over_tcp_and_records_frames(self):
        with tempfile.TemporaryDirectory() as temp, socket.create_server(("127.0.0.1", 0)) as server:
            frames_path = Path(temp) / "frames.jsonl"
            module = DimmerModule(SERIAL_NUMBER)
            link = ModuleLink(module, server.getsockname()[1], frames_path, connect_timeout=5)
            try:
                server.settimeout(5)
                emulator_side, _ = server.accept()
                with emulator_side:
                    emulator_side.settimeout(5)
                    emulator_side.sendall(build_frame(BROADCAST_ADDRESS, FRAME_DISCOVER))
                    self.assertEqual(receive_exactly(emulator_side, DISCOVERY_REPLY_LENGTH)[:3], bytes([0xAA, 0x00, 0x08]))
                    emulator_side.sendall(assign_frame(SERIAL_NUMBER, 5))
                    self.assertEqual(receive_exactly(emulator_side, len(ACK)), ACK)
                    link.wait_until(lambda: module.address == 5, 5, "address 5")
                    self.assertEqual(link.address(), 5)
            finally:
                link.close()
            rows = [json.loads(line) for line in frames_path.read_text(encoding="utf-8").splitlines()]
            self.assertEqual([row["from"] for row in rows], ["clu", "module", "clu", "module"])

    def test_closed_bridge_is_reported_to_waiters(self):
        with tempfile.TemporaryDirectory() as temp, socket.create_server(("127.0.0.1", 0)) as server:
            link = ModuleLink(DimmerModule(SERIAL_NUMBER), server.getsockname()[1], Path(temp) / "f.jsonl", 5)
            try:
                server.settimeout(5)
                emulator_side, _ = server.accept()
                emulator_side.close()
                with self.assertRaisesRegex(RuntimeError, "closed the UART1 bridge"):
                    link.wait_until(lambda: False, 5, "never")
            finally:
                link.close()


class ValidationTests(unittest.TestCase):
    def test_set_value_needs_ramp_time_first(self):
        set_value = (0x0C, 0, bytes([0, 0xF4, 0x01]))
        validate_set_value_commands([(0x06, 0, bytes(5)), set_value], 500)
        with self.assertRaisesRegex(ValueError, "preceded by SET_RAMP_TIME"):
            validate_set_value_commands([set_value, (0x06, 0, bytes(5))], 500)
        with self.assertRaisesRegex(ValueError, "no SET_VALUE"):
            validate_set_value_commands([(0x06, 0, bytes(5))], 500)

    def test_set_value_rejects_malformed_ramp_time(self):
        set_value = (0x0C, 0, bytes([0, 0xF4, 0x01]))
        validate_set_value_commands([(0x06, 0, bytes([0, 0xE8, 0x03, 0, 0])), set_value], 500)
        malformed_ramps = [
            (0x06, 0, b"\x00"),
            (0x06, 0xFF, bytes(5)),
            (0x06, 0, bytes([1, 0, 0, 0, 0])),
            (0x06, 0, bytes([0, 0, 0, 1, 0])),
            (0x06, 0, bytes(6)),
        ]
        for ramp in malformed_ramps:
            with self.subTest(ramp=ramp), self.assertRaisesRegex(ValueError, "preceded by SET_RAMP_TIME"):
                validate_set_value_commands([ramp, set_value], 500)

    def test_log_markers(self):
        good = "TFBUS_READY abc\r\nTFBUS_VALUE abc 0.5\r\nTFBUS_VALUE abc 0.25\r\n"
        validate_tfbus_log(good, "abc")
        with self.assertRaisesRegex(ValueError, "Value 0.25"):
            validate_tfbus_log("TFBUS_READY abc\nTFBUS_VALUE abc 0.5\n", "abc")
        with self.assertRaisesRegex(ValueError, "failure signature"):
            validate_tfbus_log(good + "Guru Meditation\n", "abc")


class TfbusRunnerTests(unittest.TestCase):
    def inputs(self, path: Path) -> argparse.Namespace:
        for name in ("firmware.bin", "firmware.elf", "cfg.bin"):
            (path / name).write_bytes(b"input")
        return argparse.Namespace(
            firmware=path / "firmware.bin",
            elf=path / "firmware.elf",
            cfg=path / "cfg.bin",
            out_dir=path / "tfbus-test",
            emulator="esp-emu",
            emulator_timeout=180,
            psram_size="8M",
            uart_port=19401,
        )

    def test_device_gets_the_uart1_bridge_and_both_ends_close_on_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.inputs(Path(temp))
            device = Mock()
            link = Mock()
            link.wait_until.side_effect = TimeoutError("no module")
            with patch("test.emulator.tfbus.run_tfbus.Device", return_value=device) as device_factory, patch(
                "test.emulator.tfbus.run_tfbus.ModuleLink", return_value=link
            ):
                with self.assertRaisesRegex(TimeoutError, "no module"):
                    run_test(args)
            self.assertEqual(
                device_factory.call_args.kwargs["extra_emulator_args"], ("--uart1-tcp", "127.0.0.1:19401")
            )
            device.close.assert_called_once()
            link.close.assert_called_once()

    def test_missing_input_fails_before_starting_anything(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.inputs(Path(temp))
            args.cfg.unlink()
            with patch("test.emulator.tfbus.run_tfbus.ModuleLink") as link_factory:
                with self.assertRaises(FileNotFoundError):
                    run_test(args)
            link_factory.assert_not_called()


def receive_exactly(connection: socket.socket, byte_count: int) -> bytes:
    """Read @p byte_count bytes; TCP may split or merge the module's replies."""
    received = bytearray()
    while len(received) < byte_count:
        chunk = connection.recv(byte_count - len(received))
        if not chunk:
            raise ConnectionError(f"connection closed after {len(received)} of {byte_count} bytes")
        received.extend(chunk)
    return bytes(received)


if __name__ == "__main__":
    unittest.main()
