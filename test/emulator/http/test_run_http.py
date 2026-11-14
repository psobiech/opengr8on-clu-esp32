import argparse
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, call, patch

from .run_http import run_test, validate_http_logs
from ..udp.run_udp import run_with_timeout


def successful_logs(nonce):
    logs = {}
    for name, peer in (("A", "B"), ("B", "A")):
        markers = [
            f"HTTP_READY {name} {nonce}",
            f"HTTP_DONE {name} {nonce}",
            f"HTTP_404_OK {name} {nonce}",
        ]
        markers += [f"HTTP_REPLY_OK {name} {seq} {nonce}" for seq in range(1, 4)]
        markers += [f"HTTP_RECEIVED {name} {nonce}:{peer}:post:{seq}" for seq in range(1, 4)]
        logs[name] = "\n".join(markers)
    return logs


class HttpLogTests(unittest.TestCase):
    def test_exact_bidirectional_markers_pass(self):
        validate_http_logs(successful_logs("abc123"), "abc123")

    def test_missing_reply_fails(self):
        logs = successful_logs("abc123")
        logs["B"] = logs["B"].replace("HTTP_REPLY_OK B 2 abc123\n", "")
        with self.assertRaisesRegex(ValueError, "HTTP_REPLY_OK B 2"):
            validate_http_logs(logs, "abc123")

    def test_duplicate_request_marker_fails(self):
        logs = successful_logs("abc123")
        logs["A"] += "\nHTTP_RECEIVED A abc123:B:post:1"
        with self.assertRaisesRegex(ValueError, "exactly one"):
            validate_http_logs(logs, "abc123")

    def test_failure_signature_fails_even_with_readiness_markers(self):
        logs = successful_logs("abc123")
        logs["A"] += "\nGuru Meditation"
        with self.assertRaisesRegex(ValueError, "failure signature"):
            validate_http_logs(logs, "abc123")

    def test_missing_peer_log_fails(self):
        with self.assertRaisesRegex(ValueError, "peers A and B"):
            validate_http_logs({"A": ""}, "abc123")


class HttpRunnerTests(unittest.TestCase):
    def inputs(self, path):
        firmware = path / "firmware.bin"
        elf = path / "firmware.elf"
        cfg_a = path / "cfg-a.bin"
        cfg_b = path / "cfg-b.bin"
        for item in (firmware, elf, cfg_a, cfg_b):
            item.write_bytes(b"input")
        return argparse.Namespace(
            firmware=firmware,
            elf=elf,
            cfg_a=cfg_a,
            cfg_b=cfg_b,
            out_dir=path / "http-test",
            emulator="esp-emu",
            emulator_timeout=180,
            psram_size="2M",
        )

    def device(self, path, name, logs):
        device = Mock()
        device.name = name
        device.tcp_port = 19301 if name == "A" else 19302
        device.log_path = path / f"{name}.log"
        device.log_path.write_text(logs, encoding="utf-8")
        device.process.poll.return_value = None
        device.call.side_effect = lambda script, retries=0: "started" if script.startswith("return http_start(") else "3"
        return device

    def test_run_requires_three_exchanges_each_way_and_closes_peers(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            args = self.inputs(path)
            logs = successful_logs("fixednonce")
            devices = [self.device(path, name, logs[name]) for name in ("A", "B")]
            with patch("test.emulator.http.run_http.secrets.token_hex", return_value="fixednonce"), patch(
                "test.emulator.http.run_http.Device", side_effect=devices
            ) as device_factory:
                result = run_test(args)
            self.assertIn("A -> B 3/3, B -> A 3/3", result)
            self.assertEqual(device_factory.call_args.kwargs["psram_size"], "2M")
            self.assertIn("HTTP-A.LUA", [item.name for item in args.out_dir.iterdir()])
            self.assertEqual(
                devices[0].call.call_args_list,
                [
                    call("return http_start(19302)"),
                    call("return http_completed()", retries=1),
                    call("return http_received()", retries=1),
                ],
            )
            self.assertEqual(
                devices[1].call.call_args_list,
                [
                    call("return http_received()", retries=1),
                    call("return http_start(19301)"),
                    call("return http_completed()", retries=1),
                ],
            )
            for device in devices:
                device.close.assert_called_once()

    def test_wrong_counter_still_fails_and_closes_peers(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            args = self.inputs(path)
            logs = successful_logs("fixednonce")
            devices = [self.device(path, name, logs[name]) for name in ("A", "B")]
            devices[0].call.side_effect = lambda script, retries=0: (
                "started" if script.startswith("return http_start(") else "2"
            )
            with patch("test.emulator.http.run_http.secrets.token_hex", return_value="fixednonce"), patch(
                "test.emulator.http.run_http.Device", side_effect=devices
            ):
                with self.assertRaisesRegex(RuntimeError, "not all three HttpRequest responses completed"):
                    run_test(args)
            self.assertEqual(
                devices[0].call.call_args_list,
                [call("return http_start(19302)"), call("return http_completed()", retries=1)],
            )
            for device in devices:
                device.close.assert_called_once()

    def test_second_startup_failure_closes_first_peer(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            args = self.inputs(path)
            first = Mock()
            with patch("test.emulator.http.run_http.Device", side_effect=[first, OSError("cannot start B")]):
                with self.assertRaisesRegex(OSError, "cannot start B"):
                    run_test(args)
            first.close.assert_called_once()

    def test_host_timeout_propagates_and_closes_both_peers(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            args = self.inputs(path)
            devices = [Mock(), Mock()]
            devices[0].wait_for.side_effect = lambda *unused: time.sleep(1)
            with patch("test.emulator.http.run_http.Device", side_effect=devices):
                with self.assertRaisesRegex(TimeoutError, "HTTP integration exceeded host timeout"):
                    run_with_timeout(lambda: run_test(args), 0.02, label="HTTP integration")
            for device in devices:
                device.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
