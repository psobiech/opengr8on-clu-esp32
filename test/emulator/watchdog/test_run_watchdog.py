import socket
import tempfile
import threading
import unittest
from pathlib import Path
from unittest.mock import Mock

from .run_watchdog import (
    EMERGENCY_MARKER,
    SKIPPED_MARKER,
    WATCHDOG_MARKER,
    reset_chip,
    runaway_script,
    validate_watchdog_log,
    wait_for_count,
)


NONCE = "0123456789abcdef"


def crash_boot(stuck_task: str = "IDLE1") -> str:
    """Log of one boot running the runaway script until the task watchdog panics."""
    return (
        f"WDT_LOOP {NONCE}\n"
        f"E (25000) task_wdt: {WATCHDOG_MARKER}. The following tasks/users did not reset the watchdog in time:\n"
        f"E (25000) task_wdt:  - {stuck_task} (CPU 1)\n"
        "Rebooting...\n"
        f"E (300) crash: previous boot crashed: task watchdog: {stuck_task} (CPU 1) | task clu_lua | pc 0x420a0000\n"
    )


def boot_loop_log() -> str:
    return (
        "I (900) lua: no /fs/a/MAIN.LUA, idle\n"
        + crash_boot()
        + crash_boot("clu_lua")
        + f"E (1200) lua: {SKIPPED_MARKER}\nE (1200) lua: {EMERGENCY_MARKER}\n"
    )


class ValidateWatchdogLogTests(unittest.TestCase):
    def test_two_reported_crashes_then_skipped_main_passes(self):
        validate_watchdog_log(boot_loop_log(), NONCE)

    def test_script_started_a_third_time_fails(self):
        with self.assertRaisesRegex(ValueError, "start exactly twice"):
            validate_watchdog_log(boot_loop_log() + f"WDT_LOOP {NONCE}\n", NONCE)

    def test_crash_not_reported_after_the_reset_fails(self):
        output = boot_loop_log().replace("previous boot crashed", "previous boot ok", 1)
        with self.assertRaisesRegex(ValueError, "reported on the next boot"):
            validate_watchdog_log(output, NONCE)

    def test_crash_of_another_task_is_not_a_lua_crash_report(self):
        output = boot_loop_log().replace("task watchdog: IDLE1 (CPU 1)", "task watchdog: clu_udp (CPU 0)", 1)
        with self.assertRaisesRegex(ValueError, "reported on the next boot"):
            validate_watchdog_log(output, NONCE)

    def test_main_skipped_before_the_second_crash_fails(self):
        output = f"{SKIPPED_MARKER}\n" + crash_boot() + crash_boot() + f"{EMERGENCY_MARKER}\n"
        with self.assertRaisesRegex(ValueError, "skipped exactly once|after the second crash report"):
            validate_watchdog_log(output, NONCE)

    def test_main_never_skipped_fails(self):
        output = boot_loop_log().replace(SKIPPED_MARKER, "")
        with self.assertRaisesRegex(ValueError, "skipped exactly once"):
            validate_watchdog_log(output, NONCE)

    def test_other_fault_fails(self):
        with self.assertRaisesRegex(ValueError, "LoadProhibited"):
            validate_watchdog_log(boot_loop_log() + "Guru Meditation Error: Core 1 panic'ed (LoadProhibited)\n", NONCE)


class RunawayScriptTests(unittest.TestCase):
    def test_prints_the_marker_then_never_blocks(self):
        self.assertEqual(runaway_script(NONCE), f'print("WDT_LOOP {NONCE}")\nwhile true do end\n')


class ResetChipTests(unittest.TestCase):
    def serve_once(self, reply: bytes) -> tuple[int, list[bytes], threading.Thread]:
        server = socket.socket()
        server.bind(("127.0.0.1", 0))
        server.listen(1)
        received = []

        def answer():
            with server:
                connection, _ = server.accept()
                with connection:
                    received.append(connection.makefile("rb").readline())
                    connection.sendall(reply)

        thread = threading.Thread(target=answer, daemon=True)
        thread.start()
        return server.getsockname()[1], received, thread

    def test_sends_reset_and_accepts_ok(self):
        port, received, thread = self.serve_once(b"ok\n")
        reset_chip(port)
        thread.join(5)
        self.assertEqual(received, [b"reset\n"])

    def test_error_reply_fails(self):
        port, _, thread = self.serve_once(b"error: busy\n")
        with self.assertRaisesRegex(RuntimeError, "control reset answered"):
            reset_chip(port)
        thread.join(5)

    def test_no_control_channel_fails_after_the_timeout(self):
        with socket.socket() as unused:
            unused.bind(("127.0.0.1", 0))
            port = unused.getsockname()[1]
        with self.assertRaises(ConnectionRefusedError):
            reset_chip(port, timeout=0.3)


class WaitForCountTests(unittest.TestCase):
    def device_with_log(self, text: str) -> Mock:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        log_path = Path(directory.name) / "CLU.log"
        log_path.write_text(text, encoding="utf-8")
        device = Mock(log_path=log_path)
        device.process.poll.return_value = None
        return device

    def test_returns_once_the_marker_appears_often_enough(self):
        wait_for_count(self.device_with_log("A\nA\n"), "A", 2, timeout=1)

    def test_times_out_when_seen_too_few_times(self):
        with self.assertRaisesRegex(TimeoutError, "x2"):
            wait_for_count(self.device_with_log("A\n"), "A", 2, timeout=0.3)

    def test_emulator_exit_fails_at_once(self):
        device = self.device_with_log("")
        device.process.poll.return_value = 1
        device.process.returncode = 1
        with self.assertRaisesRegex(RuntimeError, "exited with 1"):
            wait_for_count(device, "A", 1, timeout=5)


if __name__ == "__main__":
    unittest.main()
