import argparse
import itertools
import socket
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from .run_udp import (
    BROADCAST_IV,
    FACTORY_DEVICE_KEY,
    MASTER_KEY,
    Device,
    Relay,
    decrypt,
    encrypt,
    main,
    response_session,
    run_test,
    run_with_timeout,
    validate_packet_records,
)


class CryptoTests(unittest.TestCase):
    def test_aes_cbc_round_trip(self):
        plaintext = b"req:192.168.4.1:00000001:return 'ok'"
        self.assertEqual(decrypt(encrypt(plaintext, MASTER_KEY, BROADCAST_IV), MASTER_KEY, BROADCAST_IV), plaintext)

    def test_decrypt_rejects_malformed_ciphertext(self):
        with self.assertRaises(ValueError):
            decrypt(b"short", FACTORY_DEVICE_KEY, BROADCAST_IV)


class SessionTests(unittest.TestCase):
    def test_response_session_requires_response_header_and_hex_id(self):
        self.assertEqual(response_session("resp:192.168.4.1:0000000a:ok"), "0000000a")
        self.assertIsNone(response_session("req:192.168.4.1:0000000a:ignored"))
        self.assertIsNone(response_session("resp:192.168.4.1:xyz:ignored"))


class DeviceTests(unittest.TestCase):
    def setUp(self):
        self.device = object.__new__(Device)
        self.device.name = "A"
        self.device.key = MASTER_KEY
        self.device.iv = BROADCAST_IV
        self.device.session = 0
        self.device.sock = Mock()

    def response(self, session, value):
        return encrypt(f"resp:192.168.4.1:{session:08x}:{value}".encode(), MASTER_KEY, BROADCAST_IV)

    def test_call_ignores_stale_session_and_bad_ciphertext(self):
        self.device.session = 1
        self.device.sock.recv.side_effect = [b"bad", self.response(1, "stale"), self.response(2, "right:result")]
        self.assertEqual(self.device.call("return 'right:result'"), "right:result")
        self.assertEqual(self.device.sock.send.call_count, 1)

    def test_call_does_not_retry_side_effects_by_default(self):
        self.device.sock.recv.side_effect = socket.timeout
        with patch("test.emulator.udp.run_udp.time.monotonic", side_effect=itertools.count()):
            with self.assertRaises(TimeoutError):
                self.device.call("do_something()", timeout=2)
        self.assertEqual(self.device.sock.send.call_count, 1)

    def test_explicit_idempotent_retry_uses_a_new_session(self):
        self.device.sock.recv.side_effect = [socket.timeout(), self.response(2, "ok")]
        with patch("test.emulator.udp.run_udp.time.monotonic", side_effect=itertools.count()):
            self.assertEqual(self.device.call("value = 1; return 'ok'", retries=1, timeout=2), "ok")
        self.assertEqual(self.device.sock.send.call_count, 2)

    def test_explicit_retry_still_fails_after_both_replies_are_lost(self):
        self.device.sock.recv.side_effect = socket.timeout
        with patch("test.emulator.udp.run_udp.time.monotonic", side_effect=itertools.count()):
            with self.assertRaisesRegex(TimeoutError, "session 00000002 timed out"):
                self.device.call("return http_received()", retries=1, timeout=2)
        self.assertEqual(self.device.sock.send.call_count, 2)

    def test_upload_retries_only_idempotent_operations_and_loads_once(self):
        script = 'print("hello")\n'
        self.device.call = Mock(side_effect=["ok", "ok", str(len(script)), "true", "loaded"])
        self.device.upload(script)
        calls = self.device.call.call_args_list
        self.assertEqual(calls[1].args[0], 'upload_parts[1] = "print(\\"hello\\")\\n"; return "ok"')
        self.assertTrue(all(call.kwargs == {"retries": 1} for call in calls[:-1]))
        self.assertEqual(calls[-1].kwargs, {})

    def test_optional_tcp_forward_preserves_udp_forward(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            image = path / "image.bin"
            cfg = path / "cfg.bin"
            image.write_bytes(b"firmware")
            cfg.write_bytes(bytes(0x3000))
            with patch("test.emulator.udp.run_udp.socket.socket") as sock_factory, patch(
                "test.emulator.udp.run_udp.subprocess.Popen"
            ) as popen:
                popen.return_value.poll.return_value = 0
                device = Device("A", 19201, cfg, image, image, "esp-emu", path, 180, tcp_port=19301)
                command = popen.call_args.args[0]
                network = command[command.index("--net") + 1]
                self.assertIn("hostfwd=udp:127.0.0.1:19201-:1234", network)
                self.assertIn("hostfwd=tcp:127.0.0.1:19301-:80", network)
                self.assertEqual(command[command.index("--psram-size") + 1], "8M")
                self.assertEqual(device.tcp_port, 19301)
                device.close()
                sock_factory.return_value.close.assert_called_once()

    def test_tcp_forward_is_omitted_for_udp_only_device(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            image = path / "image.bin"
            cfg = path / "cfg.bin"
            image.write_bytes(b"firmware")
            cfg.write_bytes(bytes(0x3000))
            with patch("test.emulator.udp.run_udp.socket.socket"), patch(
                "test.emulator.udp.run_udp.subprocess.Popen"
            ) as popen:
                popen.return_value.poll.return_value = 0
                device = Device("A", 19201, cfg, image, image, "esp-emu", path, 180, psram_size="2M")
                network = popen.call_args.args[0][popen.call_args.args[0].index("--net") + 1]
                command = popen.call_args.args[0]
                self.assertEqual(network, "user,hostfwd=udp:127.0.0.1:19201-:1234")
                self.assertEqual(command[command.index("--psram-size") + 1], "2M")
                self.assertIsNone(device.tcp_port)
                device.close()


class LifecycleTests(unittest.TestCase):
    def test_host_timeout_interrupts_the_work(self):
        with self.assertRaisesRegex(TimeoutError, "host timeout"):
            run_with_timeout(lambda: time.sleep(1), 0.02)
        self.assertEqual(run_with_timeout(lambda: "ok", 1), "ok")

    def test_failed_rerun_replaces_stale_success_artifacts(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            (path / "result.txt").write_text("PASS: old run\n")
            (path / "packets.json").write_text('[{"old": true}]\n')
            argv = [
                "run_udp.py", "--firmware", "missing.bin", "--elf", "missing.elf",
                "--cfg-a", "a.bin", "--cfg-b", "b.bin", "--out-dir", temp,
            ]
            with patch("sys.argv", argv), patch("sys.stderr"):
                self.assertEqual(main(), 1)
            self.assertTrue((path / "result.txt").read_text().startswith("FAIL:"))
            self.assertEqual((path / "packets.json").read_text(), "[]\n")

    def test_second_startup_failure_closes_first_device(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            input_path = path / "input.bin"
            input_path.write_bytes(b"input")
            args = argparse.Namespace(
                firmware=input_path, elf=input_path, cfg_a=input_path, cfg_b=input_path,
                out_dir=path, emulator="esp-emu", emulator_timeout=180,
            )
            first = Mock()
            with patch("test.emulator.udp.run_udp.Device", side_effect=[first, OSError("cannot start B")]):
                with self.assertRaisesRegex(OSError, "cannot start B"):
                    run_test(args)
            first.close.assert_called_once()


class RelayTests(unittest.TestCase):
    def test_forwards_ciphertext_unchanged_and_can_drop_a_request(self):
        key, iv = MASTER_KEY, BROADCAST_IV
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
            peer.bind(("127.0.0.1", 0))
            peer.settimeout(2)
            relay = Relay(key, iv, bind_port=0)
            try:
                relay.target_port = peer.getsockname()[1]
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                    client.settimeout(2)
                    request = encrypt(b"req:192.168.4.1:00000001:payload", key, iv)
                    client.sendto(request, relay.listener.getsockname())
                    forwarded, address = peer.recvfrom(4096)
                    self.assertEqual(forwarded, request)
                    response = encrypt(b"resp:192.168.4.1:00000001:result", key, iv)
                    peer.sendto(response, address)
                    self.assertEqual(client.recv(4096), response)
                    relay.target_port = None
                    client.sendto(request, relay.listener.getsockname())
                    client.settimeout(0.2)
                    with self.assertRaises(socket.timeout):
                        client.recv(4096)
                self.assertEqual([row["direction"] for row in relay.records], ["request", "response", "dropped"])
                self.assertEqual(relay.errors, [])
            finally:
                relay.close()


class PacketRecordTests(unittest.TestCase):
    def setUp(self):
        self.records = [
            {"direction": "request", "plaintext": f"req:192.168.4.1:{i:08x}:payload"}
            for i in range(1, 7)
        ]
        self.records += [
            {"direction": "response", "plaintext": f"resp:192.168.4.1:{i:08x}:result"}
            for i in range(1, 7)
        ]
        self.records.append({"direction": "dropped", "plaintext": "req:192.168.4.1:00000007:lost"})

    def test_six_matched_sessions_and_dropped_request_pass(self):
        validate_packet_records(self.records)

    def test_missing_response_fails(self):
        with self.assertRaisesRegex(ValueError, "expected 6"):
            validate_packet_records(self.records[:6] + self.records[7:])

    def test_mismatched_response_session_fails(self):
        self.records[6]["plaintext"] = "resp:192.168.4.1:00000009:result"
        with self.assertRaisesRegex(ValueError, "session IDs"):
            validate_packet_records(self.records)

    def test_non_request_cannot_be_counted_as_dropped(self):
        self.records[-1]["plaintext"] = "resp:192.168.4.1:00000007:unexpected"
        with self.assertRaisesRegex(ValueError, "dropped packet"):
            validate_packet_records(self.records)


if __name__ == "__main__":
    unittest.main()
