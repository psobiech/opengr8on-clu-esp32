import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock

from .run_boot import BOOT_MARKERS, assess_boot, boot_markers, run_check


class AssessBootTests(unittest.TestCase):
    def setUp(self):
        self.ready_output = "\n".join(BOOT_MARKERS)

    def test_all_readiness_markers_and_clean_exit_pass(self):
        self.assertEqual(assess_boot(self.ready_output, 0), [])

    def test_low_memory_markers_pass_only_for_configured_sizes(self):
        low_memory_output = "\n".join(boot_markers("4MB", "2M"))
        self.assertEqual(assess_boot(low_memory_output, 0, "4MB", "2M"), [])
        failures = assess_boot(self.ready_output, 0, "4MB", "2M")
        self.assertIn("missing boot marker: SPI Flash Size : 4MB", failures)
        self.assertIn("missing boot marker: Found 2MB PSRAM device", failures)

    def test_conflicting_standard_memory_markers_fail_low_memory_check(self):
        output = "\n".join(boot_markers("4MB", "2M")) + "\nSPI Flash Size : 16MB\nFound 8MB PSRAM device"
        failures = assess_boot(output, 0, "4MB", "2M")
        self.assertIn("unexpected flash size marker: 16MB", failures)
        self.assertIn("unexpected PSRAM size marker: 8MB", failures)

    def test_partial_and_empty_boots_fail_missing_markers(self):
        partial = assess_boot("storage mounted at /fs", 0)
        self.assertIn("missing boot marker: no /fs/a/MAIN.LUA, idle", partial)
        self.assertEqual(len(assess_boot("", 0)), len(BOOT_MARKERS))

    def test_panic_after_readiness_fails(self):
        failures = assess_boot(self.ready_output + "\nGuru Meditation Error", 0)
        self.assertTrue(any("failure signature" in failure for failure in failures))

    def test_emulator_panic_intercept_notice_is_not_a_crash(self):
        output = self.ready_output + "\n[INFO machine] Panic intercept: 'abort' at 0x4037D7EC"
        self.assertEqual(assess_boot(output, 0), [])

    def test_elf_mismatch_fails(self):
        output = self.ready_output + "\nnot from any of the 1 application ELF(s) given"
        failures = assess_boot(output, 0)
        self.assertTrue(any("application ELF" in failure for failure in failures))

    def test_nonzero_exit_fails_even_after_readiness(self):
        failures = assess_boot(self.ready_output, 2)
        self.assertIn("emulator exited with status 2", failures)


class RunCheckTests(unittest.TestCase):
    def test_wall_clock_timeout_is_logged_and_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            log_path = Path(temp) / "emu.log"
            runner = Mock(
                side_effect=subprocess.TimeoutExpired("esp-emu", 2, output=b"booting")
            )
            failures = run_check(["esp-emu"], 2, log_path, runner)
            self.assertTrue(any("exceeded host timeout" in failure for failure in failures))
            self.assertIn("booting", log_path.read_text(encoding="utf-8"))

    def test_process_start_error_is_logged_and_fails(self):
        with tempfile.TemporaryDirectory() as temp:
            log_path = Path(temp) / "emu.log"
            runner = Mock(side_effect=FileNotFoundError("esp-emu"))
            failures = run_check(["esp-emu"], 2, log_path, runner)
            self.assertTrue(any("status 127" in failure for failure in failures))
            self.assertIn("could not start emulator", log_path.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()