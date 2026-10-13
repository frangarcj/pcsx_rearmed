import importlib.util
import contextlib
import io
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

MODULE_PATH = (Path(__file__).resolve().parents[1] /
               "tools/vita-harness/check_system12_gameplay_vita3k.py")
SPEC = importlib.util.spec_from_file_location("system12_vita_gameplay", MODULE_PATH)
GAMEPLAY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GAMEPLAY)


class System12VitaGameplayEvidenceTests(unittest.TestCase):
    def test_invalid_csv_fails_before_waiting_for_completion(self):
        with tempfile.TemporaryDirectory() as directory:
            fragments = ("pcsx-vita: system12-report PASS",)
            code = ("import time; print('pcsx-vita: invalid inputs CSV line=386',flush=True); "
                    "print('Stopping -> Idle',flush=True); time.sleep(10)")
            process = subprocess.Popen([sys.executable, "-u", "-c", code],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            captured = GAMEPLAY.CapturedGuest(process, Path(directory) / "log", "bad CSV", fragments)
            started = time.monotonic()
            try:
                self.assertFalse(captured.wait_for_messages(fragments, 3, stable=0.1))
                self.assertLess(time.monotonic() - started, 2)
                self.assertEqual(captured.issue_counts["pcsx-vita: invalid inputs CSV"], 1)
            finally:
                process.terminate()
                process.wait(timeout=5)
                captured.close()

    def test_csv_reader_rejects_nul_and_checks_eof_row_bounds(self):
        compiler = shutil.which("cc") or shutil.which("clang")
        if not compiler:
            self.skipTest("host C compiler is not available")
        source = (Path(__file__).resolve().parent / "vita_input_csv_reader_fixture.c")
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "input-reader-test"
            subprocess.run([compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                            str(source), "-o", str(binary)], check=True,
                           capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, timeout=5)

    @unittest.skipUnless(os.name == "posix", "process-group watchdog uses POSIX sessions")
    def test_watchdog_kills_owned_group_even_when_caller_is_stalled(self):
        with tempfile.TemporaryDirectory() as directory:
            child_pid_file = Path(directory) / "child.pid"
            child_signal_file = Path(directory) / "child.sigterm"
            child_code = (
                "import signal,time,pathlib; "
                f"signal.signal(signal.SIGTERM, lambda *_: pathlib.Path({str(child_signal_file)!r}).write_text('term')); "
                "time.sleep(30)")
            parent_code = (
                "import pathlib,subprocess,sys,time; "
                f"child=subprocess.Popen([sys.executable,'-c',{child_code!r}]); "
                f"pathlib.Path({str(child_pid_file)!r}).write_text(str(child.pid)); "
                "time.sleep(30)")
            process = subprocess.Popen([sys.executable, "-c", parent_code],
                                       start_new_session=True)
            watchdog = GAMEPLAY.ProcessGroupWatchdog(process, 0.15)
            try:
                deadline = time.monotonic() + 3
                while not child_pid_file.exists() and time.monotonic() < deadline:
                    time.sleep(0.01)
                self.assertTrue(child_pid_file.exists())
                time.sleep(2.0)  # Simulate a stalled caller through TERM and forced KILL.
            finally:
                watchdog.close()
            self.assertTrue(watchdog.timed_out)
            self.assertIsNotNone(process.poll())
            self.assertTrue(child_signal_file.exists())

    def test_png_screenshot_config_is_explicit_and_unique(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "config.yml"
            path.write_text("pref-path: /tmp/fs\nscreenshot-format: 1\n")
            GAMEPLAY.set_png_screenshot_format(path)
            self.assertIn("screenshot-format: 2\n", path.read_text())
            path.write_text("screenshot-format: 0\nscreenshot-format: 1\n")
            with self.assertRaisesRegex(RuntimeError, "duplicate screenshot-format"):
                GAMEPLAY.set_png_screenshot_format(path)

    def test_replay_capture_schedule_reaches_six_hundred_seconds(self):
        times = [float(value) for value in GAMEPLAY.GAMEPLAY_SCREENSHOT_TIMES.split(",")]
        self.assertTrue({120, 180, 240, 360, 480, 600}.issubset(times))

    def test_console_smoke_evidence_is_bound_to_exact_vpk_and_host_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "smoke.json"
            path.write_text('{"console_smoke":"PASS","vpk_sha256":"v",'
                            '"host_sha256":"h"}')
            self.assertEqual(GAMEPLAY.validate_smoke_evidence(path, "v", "h")[
                "console_smoke"], "PASS")
            with self.assertRaisesRegex(RuntimeError, "does not match"):
                GAMEPLAY.validate_smoke_evidence(path, "changed", "h")
            path.write_text('{"console_smoke":"FAIL","vpk_sha256":"v",'
                            '"host_sha256":"h"}')
            with self.assertRaisesRegex(RuntimeError, "does not match"):
                GAMEPLAY.validate_smoke_evidence(path, "v", "h")

    def test_native_screenshot_copy_requires_real_png_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "portable" / "screenshots"
            source = root / "title" / "frame.png"
            source.parent.mkdir(parents=True)
            valid_header = (b"\x89PNG\r\n\x1a\n" + (13).to_bytes(4, "big") + b"IHDR" +
                            (1).to_bytes(4, "big") * 2 + bytes((8, 6, 0, 0, 0)))
            source.write_bytes(valid_header + b"fixture")
            output = Path(directory) / "out"
            output.mkdir()
            copied = GAMEPLAY.copy_new_screenshots(
                root, set(), output, 400)
            self.assertEqual(len(copied), 1)
            self.assertEqual(Path(copied[0]["path"]).read_bytes(), source.read_bytes())
            source.write_bytes(b"not a png")
            bad_output = Path(directory) / "bad"
            bad_output.mkdir()
            with self.assertRaisesRegex(RuntimeError, "signature/IHDR"):
                GAMEPLAY.copy_new_screenshots(root, set(), bad_output, 800)

    def test_failed_run_keeps_native_and_source_evidence_before_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            native = root / "screenshots"
            native.mkdir()
            header = (b"\x89PNG\r\n\x1a\n" + (13).to_bytes(4, "big") + b"IHDR" +
                      (1).to_bytes(4, "big") * 2)
            (native / "last.png").write_bytes(header + b"fixture")
            guest = root / "guest"
            guest.mkdir()
            (guest / "system12-12000.txt").write_text("partial report")
            output = root / "output"
            output.mkdir()
            screenshots, error = GAMEPLAY.preserve_guest_capture(
                native, set(), guest, output, 12000, False, {"|C|": 1})
            self.assertIsNone(error)
            self.assertEqual(len(screenshots), 1)
            shutil.rmtree(native)
            shutil.rmtree(guest)
            evidence = json.loads((output / "capture-evidence-12000.json").read_text())
            self.assertFalse(evidence["guest_completed"])
            self.assertEqual(evidence["runtime_issues"], {"|C|": 1})
            self.assertEqual((output / "system12-12000.txt").read_text(), "partial report")
            self.assertTrue(Path(evidence["native_screenshots"][0]["path"]).is_file())

    def test_short_guest_markers_are_visible_and_late_fatal_invalidates(self):
        fragments = ("pcsx-vita: system12-report PASS",)
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "vita.log"
            code = ("import time; print('pcsx-vita: system12-report PASS',flush=True); "
                    "print('Stopping -> Idle',flush=True); time.sleep(10)")
            process = subprocess.Popen([sys.executable, "-u", "-c", code],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            captured = GAMEPLAY.CapturedGuest(process, log, "unit pass", fragments)
            try:
                with self.assertRaisesRegex(ValueError, "exactly match"):
                    captured.wait_for_messages((), 0.1, stable=0)
                with self.assertRaisesRegex(ValueError, "exactly match"):
                    captured.wait_for_messages(("different token",), 0.1, stable=0)
                self.assertTrue(captured.wait_for_messages(fragments, 2, stable=0.15))
            finally:
                process.terminate()
                process.wait(timeout=5)
                captured.close()

            code = ("import time; print('pcsx-vita: system12-report PASS',flush=True); "
                    "print('Stopping -> Idle',flush=True); time.sleep(.15); "
                    "print('[InterpreterFallback] Unimplemented instruction at 0x1',flush=True); "
                    "time.sleep(10)")
            process = subprocess.Popen([sys.executable, "-u", "-c", code],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            captured = GAMEPLAY.CapturedGuest(process, log, "unit late fatal", fragments)
            try:
                self.assertFalse(captured.wait_for_messages(fragments, 2, stable=0.5))
            finally:
                process.terminate()
                process.wait(timeout=5)
                captured.close()
            self.assertEqual(captured.issue_counts[
                "[InterpreterFallback] Unimplemented instruction"], 1)

    def test_guest_log_filter_bounds_repeated_arm_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "vita.log"
            code = (
                "import sys; "
                "[print('[InterpreterFallback] Unimplemented instruction at 0x81155B94') "
                "for _ in range(30000)]; "
                "print('pcsx-vita: system12-report PASS path=report.txt'); "
                "print('Stopping -> Idle')")
            process = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT)
            captured = GAMEPLAY.CapturedGuest(process, log, "unit flood")
            process.wait(timeout=10)
            captured.close()
            self.assertEqual(captured.issue_counts[
                "[InterpreterFallback] Unimplemented instruction"], 30000)
            self.assertIn("Stopping -> Idle", captured.markers)
            self.assertLess(log.stat().st_size, 2 * 1024 * 1024)

    def test_private_host_snapshots_are_live_and_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "vita.log"
            code = (
                "print('Vita3K lwcond trace snapshot=1'); "
                "print('Vita3K long renderer delay delay_us=1000000 ' + 'x'*2000); "
                "[print('Vita3K lwcond trace seq=' + str(i)) for i in range(5)]; "
                "print('pcsx-vita: system12-report PASS'); print('Stopping -> Idle')")
            live = io.StringIO()
            with patch.object(GAMEPLAY.CapturedGuest, "LIVE_DIAGNOSTIC_LIMIT", 2), \
                    contextlib.redirect_stdout(live):
                process = subprocess.Popen([sys.executable, "-u", "-c", code],
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                captured = GAMEPLAY.CapturedGuest(process, log, "unit live probes")
                try:
                    process.wait(timeout=5)
                    captured.thread.join(timeout=5)
                    self.assertFalse(captured.thread.is_alive())
                    # Probe output is available before final log publication.
                    self.assertFalse(log.exists())
                    lines = live.getvalue().splitlines()
                    self.assertEqual(len(lines), 3)
                    self.assertIn("snapshot=1", lines[0])
                    self.assertIn("delay_us=1000000", lines[1])
                    self.assertLessEqual(len(lines[1]), 1024)
                    self.assertIn("limit reached", lines[2])
                    self.assertIn("Stopping -> Idle", captured.markers)
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=5)
                    captured.close()
            # Suppressing live output does not suppress the retained evidence.
            self.assertIn("Vita3K lwcond trace seq=4", log.read_text())
            self.assertIn("Vita3K long renderer delay", log.read_text())

    def test_timestamped_guest_allocator_probe_is_live(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "vita.log"
            probe = "[13:00:00] |I| [export_sceClibPrintf]: pcsx-vita: VGL_ALLOC event=0 size=8192"
            process = subprocess.Popen([sys.executable, "-u", "-c",
                                        "print(" + repr(probe) + ")"],
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            live = io.StringIO()
            with contextlib.redirect_stdout(live):
                captured = GAMEPLAY.CapturedGuest(process, log, "unit guest allocator")
                try:
                    process.wait(timeout=5)
                    captured.thread.join(timeout=5)
                    self.assertFalse(captured.thread.is_alive())
                    self.assertEqual(live.getvalue().strip(), probe)
                    self.assertFalse(log.exists())
                finally:
                    if process.poll() is None:
                        process.kill()
                        process.wait(timeout=5)
                    captured.close()
            self.assertIn(probe, log.read_text())

    def test_ppm_dimensions_and_payload_are_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "frame.ppm"
            path.write_bytes(b"P6\n2 1\n255\n\xff\x00\x00\x00\xff\x00")
            self.assertEqual(GAMEPLAY.check_capture(path)["width"], 2)
            self.assertEqual(GAMEPLAY.check_capture(path)["sampled_unique_rgb"], 2)
            self.assertFalse(GAMEPLAY.check_capture(path)["looks_black"])
            path.write_bytes(b"P6\n2 1\n255\n\x00")
            with self.assertRaisesRegex(RuntimeError, "dimensions or payload"):
                GAMEPLAY.check_capture(path)
            path.write_bytes(b"P6\n1 1\n255\n\x00\x00\x00")
            self.assertTrue(GAMEPLAY.check_capture(path)["looks_black"])

    def test_report_requires_live_board_and_h8_progress(self):
        report = (
            "System12: JAMMA=ff/bf/ff/ff\n"
            "System12: profile=soulclbrac machine=coh700 state=namcos12_state RAM=4194304 "
            "main=4194304 banked=33554432 sub=524288 wave=16777216 bank=0 alt=0\n"
            "System12: H8 pc=000100 er7=00000000 ccr=00 cycles=100 instructions=20 "
            "halted=0 illegal=000000:0000 unsupported=0 shared r=1 w=2\n"
            "System12: H8 SCI0=00/00/00 SCI1=00/00/00 IRQ=00/00/00 portB=ff\n"
            "System12: C352 rate=88200 frames=300 peak=100 WAV_bytes=0\n")
        report += (
            "System12: unmapped reads=0 writes=0 last=00000000 pc=00000000\n"
            "System12: CPU pc=80010000 opcode=00000000 ra=00000000; clock=50000000 "
            "cycle_multiplier=200 timing=source-experiment\n")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "board.txt"
            path.write_text(report)
            parsed = GAMEPLAY.read_report(path)
            self.assertEqual(parsed["records"]["H8"]["cycles"], "100")
            self.assertEqual(parsed["records"]["H8"]["instructions"], "20")
            GAMEPLAY.read_report(path, require_replay=True)
            path.write_text(report.replace("JAMMA=ff/bf/ff/ff", "JAMMA=ff/ff/ff/ff"))
            with self.assertRaisesRegex(RuntimeError, "terminal replay input"):
                GAMEPLAY.read_report(path, require_replay=True)
            path.write_text(report.replace("unsupported=0", "unsupported=1"))
            with self.assertRaisesRegex(RuntimeError, "H8 did not make"):
                GAMEPLAY.read_report(path)
            path.write_text(report.replace("machine=coh700", "machine=wrong"))
            with self.assertRaisesRegex(RuntimeError, "wrong live board profile"):
                GAMEPLAY.read_report(path)

    def test_vita_replay_csv_matches_the_177_frame_ordered_events(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "inputs.csv"
            events = GAMEPLAY.write_gameplay_replay(path)
            lines = path.read_text(encoding="ascii").splitlines()
            self.assertEqual(len(events), 177)
            self.assertEqual(lines[0], "frame,control,pressed")
            self.assertEqual(len(lines) - 1, 177)
            parsed = [tuple([int(row[0]), row[1], int(row[2])])
                      for row in (line.split(",") for line in lines[1:])]
            self.assertEqual(parsed, sorted(parsed))
            self.assertEqual(parsed[-1], (11999, "p1button3", 1))


if __name__ == "__main__":
    unittest.main()
