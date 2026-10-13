#!/usr/bin/env python3
"""Capture local Soul Calibur System 12 progress in an isolated Vita3K profile.

No game ROMs or PlayStation BIOS are downloaded. The prepared board regions are
validated against their local MAME manifest before they are copied into a VPK.
"""
from __future__ import annotations

import argparse
from collections import Counter, deque
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import threading
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TITLE_ID = "S12GAME01"
SMOKE_ID = "PCSXN0012"
PROFILE = ROOT / "artifacts/system12-profile"
DRC_CHECK = ROOT / "tools/vita-harness/check_system12_drc_vita3k.py"
GAME_FILES = ("board.cfg", "main.bin", "banked.bin", "sub.bin", "wave.bin")
MILESTONES = (400, 800, 1800)
DIAGNOSTIC_MILESTONES = MILESTONES + (3600,)
GAMEPLAY_REPLAY_FRAMES = 12000
GAMEPLAY_REPLAY_FRAME_CHOICES = (3600, 6000, GAMEPLAY_REPLAY_FRAMES)
AUTO_SCREENSHOT_TIMES = "0.5,1,2,5,10,20,30,45,60"
GAMEPLAY_SCREENSHOT_TIMES = AUTO_SCREENSHOT_TIMES + ",90,120,150,180,210,240,270,300,330,360,390,420,450,480,540,600"


class ProcessGroupWatchdog:
    """Kill only a Vita3K process group we created, even if the caller stalls."""
    def __init__(self, process, timeout: float):
        self.process = process
        self.timeout = timeout
        self.timed_out = False
        watcher = (
            "import os,signal,sys,time\n"
            "pgid=int(sys.argv[1]); timeout=float(sys.argv[2])\n"
            "time.sleep(timeout)\n"
            "try: os.killpg(pgid, 0)\n"
            "except (ProcessLookupError, PermissionError): sys.exit(0)\n"
            "try: os.killpg(pgid, signal.SIGTERM)\n"
            "except ProcessLookupError: sys.exit(0)\n"
            "deadline=time.monotonic()+1.5\n"
            "while time.monotonic()<deadline:\n"
            " try: os.killpg(pgid, 0)\n"
            " except (ProcessLookupError, PermissionError): sys.exit(23)\n"
            " time.sleep(.05)\n"
            "try: os.killpg(pgid, signal.SIGKILL)\n"
            "except ProcessLookupError: pass\n"
            "sys.exit(23)\n")
        self.helper = subprocess.Popen(
            [sys.executable, "-c", watcher, str(process.pid), str(timeout)],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL, start_new_session=True)

    def _signal_group(self, sig):
        try:
            os.killpg(self.process.pid, sig)
            return
        except (ProcessLookupError, PermissionError):
            pass
        if self.process.poll() is None:
            try:
                self.process.send_signal(sig)
            except ProcessLookupError:
                pass

    def _terminate_group(self):
        self._signal_group(signal.SIGTERM)
        deadline = time.monotonic() + 1.5
        while time.monotonic() < deadline:
            try:
                os.killpg(self.process.pid, 0)
            except (ProcessLookupError, PermissionError):
                break
            time.sleep(0.05)
        self._signal_group(signal.SIGKILL)
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()

    def close(self):
        if self.helper.poll() is None:
            self.helper.terminate()
            try:
                self.helper.wait(timeout=1)
            except subprocess.TimeoutExpired:
                self.helper.kill()
                self.helper.wait()
        self.timed_out = self.helper.returncode == 23
        self._terminate_group()


class CapturedGuest:
    """Drain Vita3K output without retaining its high-volume diagnostic lines."""
    LIVE_DIAGNOSTIC_PREFIXES = (b"Vita3K lwcond trace", b"Vita3K delay trace",
                                b"Vita3K long renderer delay")
    LIVE_DIAGNOSTIC_LIMIT = 8192
    MARKERS = (b"Stopping -> Idle", b"pcsx-vita: smoke PASS",
               b"pcsx-vita: system12-report PASS", b"pcsx-vita: system12-capture PASS",
               b"pcsx-vita: cpu_driver=ari64", b"installed successfully!")
    ISSUE_MARKERS = (b"Unhandled EXC_", b"|C|", b"[InterpreterFallback] Unimplemented instruction",
                     b"Unpredictable instruction", b"pcsx-vita: system12-report FAIL",
                     b"pcsx-vita: system12-capture FAIL", b"pcsx-vita: display detach failed",
                     b"pcsx-vita: smoke FAIL", b"pcsx-vita: inputs replay FAIL",
                     b"pcsx-vita: invalid inputs CSV",
                     b"pcsx-vita: vitaGL shutdown FAIL", b"pcsx-vita: vitaGL readback FAIL")
    KEY_WORDS = (b"PCSX staging", b"pcsx-vita:", b"Stopping -> Idle", b"installed successfully!",
                 b"[InterpreterFallback] Unimplemented instruction", b"Unpredictable instruction",
                 b"Unhandled EXC_", b"|C|", b"Successfully saved screenshot",
                 b"Failed to save screenshot", b"Failed to take screenshot",
                 b"Game started", b"Dynarmic forwarding EXC_BAD_ACCESS",
                 b"Vita3K recovered protected access", b"Vita3K protection miss",
                 b"Vita3K lwcond trace",
                 b"Vita3K delay trace",
                 b"Vita3K long renderer delay",
                 b"vitaGL-shutdown stage=")
    FORWARDED = re.compile(rb"Dynarmic forwarding EXC_BAD_ACCESS outside JIT registry pc=(0x[0-9a-fA-F]+)\s*$")
    RECOVERED = re.compile(rb"Vita3K recovered protected access pc=(0x[0-9a-fA-F]+) addr=0x[0-9a-fA-F]+ write=[01]\s*$")

    def __init__(self, process, log: Path, label: str, expected_tokens=()):
        self.process = process
        self.log = log
        self.label = label
        self.expected_tokens = tuple(token.encode("utf-8") if isinstance(token, str) else token
                                     for token in expected_tokens)
        self.expected_tokens_seen = set()
        self.markers = set()
        self.issue_counts = {marker.decode("ascii", errors="replace"): 0
                             for marker in self.ISSUE_MARKERS}
        self.issue_examples = {}
        self.lines_seen = 0
        self.bytes_seen = 0
        self.kept = []
        self.messages = deque(maxlen=128)
        self.kept_bytes = 0
        self.pending_faults = Counter()
        self.forwarded_faults = 0
        self.matched_faults = 0
        self.live_diagnostic_lines = 0
        self.thread = threading.Thread(target=self._pump, daemon=True)
        self.thread.start()

    def _record(self, raw_line: bytes):
        line = raw_line.decode("utf-8", errors="replace").rstrip("\r")
        encoded = raw_line
        self.lines_seen += 1
        if (b"pcsx-vita: PERF frames=" in encoded or
                b"pcsx-vita: cpu_driver=" in encoded or
                b"pcsx-vita: inputs replay PASS " in encoded):
            # Independent host clock: calibrate guest elapsed time between milestones.
            stamp = {"host_monotonic_ns": time.monotonic_ns(),
                     "label": self.label, "line": line[:4096]}
            with self.log.with_suffix(".perf.jsonl").open("a", encoding="utf-8") as out:
                out.write(json.dumps(stamp) + "\n")
        if b"pcsx-vita: system12-progress " in encoded:
            # Keep bounded frame milestones visible while the guarded guest runs.
            print(line[:1024], flush=True)
        if (encoded.startswith(self.LIVE_DIAGNOSTIC_PREFIXES) or
                b"PCSX staging " in encoded or
                b"pcsx-vita: VGL_ALLOC " in encoded or
                b"pcsx-vita: PERF " in encoded):
            # Private host and guest probes already bound their output. Bound it here too,
            # and expose snapshots before a stalled guest's watchdog expires.
            if self.live_diagnostic_lines < self.LIVE_DIAGNOSTIC_LIMIT:
                print(line[:1024], flush=True)
            elif self.live_diagnostic_lines == self.LIVE_DIAGNOSTIC_LIMIT:
                print("Vita3K live diagnostic limit reached; further live probe lines suppressed",
                      flush=True)
            self.live_diagnostic_lines += 1
        for token in self.expected_tokens:
            if token in encoded:
                self.expected_tokens_seen.add(token)
        forwarded = self.FORWARDED.search(encoded)
        recovered = self.RECOVERED.search(encoded)
        if forwarded:
            self.pending_faults[int(forwarded[1], 16)] += 1
            self.forwarded_faults += 1
        elif b"Dynarmic forwarding EXC_BAD_ACCESS" in encoded:
            key = "Malformed forwarded EXC_BAD_ACCESS"
            self.issue_counts[key] = self.issue_counts.get(key, 0) + 1
            self.issue_examples.setdefault(key, line[:2048])
        if recovered:
            pc = int(recovered[1], 16)
            if self.pending_faults[pc]:
                self.pending_faults[pc] -= 1
                self.matched_faults += 1
        for marker in self.MARKERS:
            if marker in encoded:
                self.markers.add(marker.decode("ascii"))
        for marker in self.ISSUE_MARKERS:
            if marker in encoded:
                key = marker.decode("ascii", errors="replace")
                self.issue_counts[key] += 1
                self.issue_examples.setdefault(key, line[:2048])
        keep = any(word in encoded for word in self.KEY_WORDS)
        if (b"pcsx-vita:" in encoded or b"installed successfully!" in encoded or
                b"screenshot" in encoded or b"Game started" in encoded):
            self.messages.append(line[:1024])
        if keep and self.kept_bytes < 2 * 1024 * 1024:
            self.kept.append(line[:4096])
            self.kept_bytes += len(line)

    def _pump(self):
        pending = b""
        stream = self.process.stdout
        try:
            while True:
                chunk = stream.read1(65536)
                if not chunk:
                    break
                self.bytes_seen += len(chunk)
                pending += chunk
                if len(pending) > 65536 and b"\n" not in pending:
                    self._record(pending[:65536])
                    pending = pending[-4096:]
                lines = pending.split(b"\n")
                pending = lines.pop()
                for line in lines:
                    self._record(line)
            if pending:
                self._record(pending)
        finally:
            stream.close()

    def wait_for_messages(self, fragments, timeout, stable=0.75, require_idle=True):
        fragments = tuple(token.encode("utf-8") if isinstance(token, str) else token
                          for token in fragments)
        if not fragments or fragments != self.expected_tokens:
            raise ValueError("wait fragments must exactly match the expected tokens captured at startup")
        deadline = time.monotonic() + timeout
        stable_at = None
        while time.monotonic() < deadline:
            if any(self.issue_counts.values()):
                return False
            matches = all(token in self.expected_tokens_seen for token in self.expected_tokens)
            idle = "Stopping -> Idle" in self.markers
            if matches and (idle or not require_idle) and not any(self.pending_faults.values()):
                if stable_at is None:
                    stable_at = time.monotonic()
                if time.monotonic() - stable_at >= stable:
                    return True
            else:
                stable_at = None
            if self.process.poll() is not None:
                return False
            time.sleep(0.05)
        return False

    def close(self):
        self.thread.join(timeout=10)
        unresolved = sum(self.pending_faults.values())
        if unresolved:
            key = "Unresolved forwarded EXC_BAD_ACCESS"
            self.issue_counts[key] = unresolved
            self.issue_examples[key] = ", ".join(
                f"pc=0x{pc:x} count={count}" for pc, count in self.pending_faults.items() if count)
        self.log.parent.mkdir(parents=True, exist_ok=True)
        with self.log.open("a", encoding="utf-8") as out:
            out.write(f"\n--- Vita3K {self.label}: {self.bytes_seen} bytes, "
                      f"{self.lines_seen} lines captured ---\n")
            for line in self.kept:
                out.write(line + "\n")
            if self.forwarded_faults:
                out.write(f"forwarded faults={self.forwarded_faults} matched recoveries={self.matched_faults} "
                          f"unresolved={unresolved}\n")
            for key, count in self.issue_counts.items():
                if count:
                    out.write(f"diagnostic count={count} marker={key}\n")
                    out.write(f"diagnostic first={self.issue_examples[key]}\n")


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import helper {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate_profile():
    checker = load_module("system12_gameplay_profile", ROOT / "tests/check_system12_gameplay.py")
    return checker.verify_profile()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def emulator_executable(path: Path) -> Path:
    starter = load_module("system12_drc_helper", DRC_CHECK)
    _bundle, relative = starter.locate_app_bundle(path)
    return path / relative if path.is_dir() else path


def validate_smoke_evidence(path: Path, vpk_hash: str, host_hash: str):
    evidence = json.loads(path.read_text(encoding="utf-8"))
    if (not isinstance(evidence, dict) or evidence.get("console_smoke") != "PASS" or
            evidence.get("vpk_sha256") != vpk_hash or
            evidence.get("host_sha256") != host_hash):
        raise RuntimeError("console-smoke evidence does not match this VPK and Vita3K executable")
    return evidence


def write_gameplay_replay(path: Path, frames=GAMEPLAY_REPLAY_FRAMES):
    replay = load_module("system12_gameplay_replay", ROOT / "tests/check_system12_gameplay.py")
    events = replay.replay_events(frames)
    if frames == GAMEPLAY_REPLAY_FRAMES and len(events) != 177:
        raise RuntimeError(f"expected the audited 177 input events, got {len(events)}")
    replay.write_events(path, events)
    return events


def make_vpk(base_vpk: Path, packer: Path, mksfoex: Path, output: Path,
             package_root: Path, title_id: str, title: str, files: list[tuple[Path, str]]):
    staging = output.parent / (output.stem + "-stage")
    staging.mkdir()
    with zipfile.ZipFile(base_vpk) as archive:
        archive.extract("sce_sys/param.sfo", staging)
        archive.extract("eboot.bin", staging)
    sfo = staging / "sce_sys/param.sfo"
    subprocess.run([str(mksfoex), "-s", f"TITLE_ID={title_id}", "-d", "ATTRIBUTE2=12",
                    title, str(sfo)], check=True, capture_output=True, text=True, timeout=30)
    command = [str(packer), "-s", str(sfo), "-b", str(staging / "eboot.bin")]
    for source, guest_name in files:
        command += ["-a", f"{source.resolve()}={guest_name}"]
    command.append(str(output))
    subprocess.run(command, check=True, capture_output=True, text=True, timeout=180)
    return output


def install_vpk(emulator: Path, vpk: Path, title_id: str, log: Path, timeout: float):
    process = subprocess.Popen([str(emulator), "--keep-config", "--log-level", "1",
                                "--", str(vpk.resolve())], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, start_new_session=True)
    watchdog = ProcessGroupWatchdog(process, timeout)
    expected_tokens = (f"[{title_id}] installed successfully!",)
    captured = CapturedGuest(process, log, f"install {title_id}", expected_tokens)
    try:
        installed = captured.wait_for_messages(expected_tokens, timeout,
                                               stable=0.25, require_idle=False)
    finally:
        watchdog.close()
        captured.close()
    accepted_returncodes = (0, -signal.SIGTERM, -signal.SIGKILL)
    return (installed and not watchdog.timed_out and
            not any(captured.issue_counts.values()) and
            process.returncode in accepted_returncodes)


def run_guest(emulator: Path, app_args: str, log: Path, timeout: float,
              done_tokens: tuple[str, ...], extra_env=None):
    environment = os.environ.copy()
    if extra_env:
        environment.update(extra_env)
    process = subprocess.Popen([str(emulator), "--keep-config", "--log-level", "1",
                                "--installed-path", TITLE_ID, "--app-args", app_args],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               env=environment, start_new_session=True)
    watchdog = ProcessGroupWatchdog(process, timeout)
    captured = CapturedGuest(process, log, f"game args={app_args}", done_tokens)
    natural_exit = False
    try:
        passed = captured.wait_for_messages(done_tokens, timeout)
    finally:
        natural_exit = process.poll() is not None
        watchdog.close()
        captured.close()
    errors = {key: count for key, count in captured.issue_counts.items() if count}
    accepted_returncodes = (0, -signal.SIGTERM, -signal.SIGKILL)
    passed = (passed and not watchdog.timed_out and
              process.returncode in accepted_returncodes and
              (not natural_exit or process.returncode == 0) and not errors)
    return passed, errors


def read_report(report_path: Path, require_replay=False) -> dict:
    lines = report_path.read_text(errors="strict").splitlines()
    records = {}
    for line in lines:
        if not line.startswith("System12: "):
            continue
        fields = line[len("System12: "):].split()
        if not fields:
            continue
        record = fields[0].split("=", 1)[0]
        values = {}
        for field in fields:
            if "=" in field:
                key, value = field.split("=", 1)
                values[key] = value
        records.setdefault(record, {}).update(values)
    if "profile" not in records or "H8" not in records or "C352" not in records:
        raise RuntimeError(f"board report is incomplete: {report_path}")
    profile = records["profile"]
    if (profile.get("profile") != "soulclbrac" or profile.get("machine") != "coh700" or
            profile.get("RAM") != "4194304" or profile.get("main") != "4194304" or
            profile.get("banked") != "33554432" or profile.get("sub") != "524288" or
            profile.get("wave") != "16777216"):
        raise RuntimeError(f"wrong live board profile in report: {report_path}")
    h8 = records["H8"]
    if h8.get("illegal") != "000000:0000":
        raise RuntimeError(f"H8 reported an illegal instruction: {report_path}")
    if (int(h8.get("cycles", "0")) <= 0 or int(h8.get("instructions", "0")) <= 0 or
            h8.get("unsupported") != "0"):
        raise RuntimeError(f"H8 did not make supported execution progress: {report_path}")
    if int(records["C352"].get("frames", "0")) <= 0:
        raise RuntimeError(f"C352 did not advance: {report_path}")
    if records.get("unmapped", {}).get("reads") != "0" or \
            records.get("unmapped", {}).get("writes") != "0":
        raise RuntimeError(f"guest touched unmapped board addresses: {report_path}")
    if require_replay and records.get("JAMMA", {}).get("JAMMA") != "ff/bf/ff/ff":
        raise RuntimeError(f"terminal replay input did not reach JAMMA port 1: {report_path}")
    if records.get("CPU", {}).get("clock") != "50000000":
        raise RuntimeError(f"unexpected System12 source-clock profile: {report_path}")
    return {"lines": lines, "records": records}


def check_capture(path: Path) -> dict:
    data = path.read_bytes()
    if not data.startswith(b"P6\n"):
        raise RuntimeError(f"capture is not a P6 PPM: {path}")
    header, _, payload = data.partition(b"\n255\n")
    parts = header.split()
    if len(parts) != 3 or parts[0] != b"P6":
        raise RuntimeError(f"bad PPM header: {path}")
    width, height = int(parts[1]), int(parts[2])
    if not 1 <= width <= 1024 or not 1 <= height <= 512 or len(payload) != width * height * 3:
        raise RuntimeError(f"bad PPM dimensions or payload: {path}")
    pixel_count = width * height
    sample_step = max(1, pixel_count // 65536)
    colors = set()
    nonblack = 0
    sampled = 0
    for i in range(0, pixel_count, sample_step):
        rgb = payload[i * 3:i * 3 + 3]
        colors.add(rgb)
        sampled += 1
        nonblack += bool(rgb[0] or rgb[1] or rgb[2])
    return {"width": width, "height": height, "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "sampled_unique_rgb": len(colors), "sampled_nonblack": nonblack,
            "sampled_pixels": sampled, "looks_black": nonblack == 0}


def read_cli(base, args):
    starter = load_module("system12_drc_helper", DRC_CHECK)
    source_bundle, binary_relative = starter.locate_app_bundle(args.emulator)
    isolated_bundle = base / source_bundle.name
    shutil.copytree(source_bundle, isolated_bundle, symlinks=True)
    emulator = isolated_bundle / binary_relative

    portable = base / "portable"
    portable.mkdir()
    source_fs = args.firmware_fs.resolve()
    config_template = (args.config_template or source_fs.parent / "config.yml").resolve()
    fs = portable / "fs"
    starter.make_isolated_config(portable / "config.yml", fs, source_fs, config_template)
    config_path = portable / "config.yml"
    set_png_screenshot_format(config_path)
    return emulator, fs, portable


def set_png_screenshot_format(config_path: Path):
    lines = config_path.read_text(encoding="utf-8").splitlines()
    keys = [i for i, line in enumerate(lines) if line.startswith("screenshot-format:")]
    if len(keys) > 1:
        raise RuntimeError("Vita3K config has duplicate screenshot-format keys")
    if keys:
        lines[keys[0]] = "screenshot-format: 2"
    else:
        lines.append("screenshot-format: 2")
    config_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def copy_new_screenshots(screenshot_root: Path, before: set[Path], output_dir: Path, frames: int):
    after = set(screenshot_root.rglob("*.png")) if screenshot_root.exists() else set()
    new_files = sorted(after - before, key=lambda path: (path.stat().st_mtime_ns, str(path)))
    if not new_files:
        raise RuntimeError(f"Vita3K produced no native PNG screenshot by frame milestone {frames}")
    destination = output_dir / f"native-screenshots-{frames}"
    destination.mkdir()
    copied = []
    for index, path in enumerate(new_files, start=1):
        with path.open("rb") as image_file:
            header = image_file.read(24)
        if (len(header) != 24 or header[:8] != b"\x89PNG\r\n\x1a\n" or
                int.from_bytes(header[8:12], "big") != 13 or header[12:16] != b"IHDR"):
            raise RuntimeError(f"Vita3K screenshot has no valid PNG signature/IHDR: {path}")
        width = int.from_bytes(header[16:20], "big")
        height = int.from_bytes(header[20:24], "big")
        if not width or not height:
            raise RuntimeError(f"Vita3K screenshot has invalid PNG dimensions: {path}")
        target = destination / f"{index:02d}-{path.name}"
        shutil.copy2(path, target)
        copied.append({"path": str(target), "width": width, "height": height,
                       "bytes": target.stat().st_size,
                       "sha256": hashlib.sha256(target.read_bytes()).hexdigest()})
    return copied


def preserve_guest_capture(screenshot_root, before, guest_data, output_dir, frames,
                           guest_completed, runtime_issues):
    """Preserve evidence before temporary-profile cleanup, including failures."""
    screenshot_error = None
    try:
        screenshots = copy_new_screenshots(screenshot_root, before, output_dir, frames)
    except (OSError, RuntimeError) as error:
        screenshots = []
        screenshot_error = str(error)
    files = []
    for suffix in ("txt", "ppm"):
        source = guest_data / f"system12-{frames}.{suffix}"
        if source.is_file():
            target = output_dir / source.name
            shutil.copy2(source, target)
            files.append(target.name)
    (output_dir / f"capture-evidence-{frames}.json").write_text(json.dumps({
        "requested_frames": frames, "guest_completed": guest_completed,
        "runtime_issues": runtime_issues, "source_files": files,
        "native_screenshots": screenshots, "native_screenshot_error": screenshot_error,
    }, indent=2) + "\n", encoding="utf-8")
    return screenshots, screenshot_error


def select_milestones(frames=None, gameplay_replay=False, replay_frames=None):
    if gameplay_replay and frames is not None:
        raise ValueError("--frames cannot be combined with --gameplay-replay")
    if replay_frames is not None and not gameplay_replay:
        raise ValueError("--replay-frames requires --gameplay-replay")
    if gameplay_replay:
        selected = GAMEPLAY_REPLAY_FRAMES if replay_frames is None else replay_frames
        if selected not in GAMEPLAY_REPLAY_FRAME_CHOICES:
            raise ValueError(f"--replay-frames must be one of {', '.join(map(str, GAMEPLAY_REPLAY_FRAME_CHOICES))}")
        return (selected,)
    if frames is None:
        return MILESTONES
    if frames not in DIAGNOSTIC_MILESTONES:
        raise ValueError(f"--frames must be one of {', '.join(map(str, DIAGNOSTIC_MILESTONES))}")
    return (frames,)


def write_results(path, metadata, milestones, completed_milestones,
                  status, failed_milestone=None):
    """Persist cumulative progress, including the first failure, for review."""
    payload = dict(metadata)
    payload.update({
        "status": status,
        "completed_milestones": list(completed_milestones),
        "failed_milestone": failed_milestone,
        "host_execution_issues": any(result.get("runtime_issues") for result in milestones),
        "milestones": milestones,
    })
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", type=Path, required=True, help="fixed Vita3K.app or its executable")
    parser.add_argument("--vpk", type=Path, required=True, help="ARCADE_SYSTEM12=1 VPK")
    parser.add_argument("--renderer", choices=("neon", "vitagl"), default="neon")
    parser.add_argument("--shader-compiler", type=Path,
                        help="existing local libshacccg.suprx; required for VitaGL")
    parser.add_argument("--firmware-fs", type=Path, required=True,
                        help="local Vita3K filesystem containing vs0, os0, sa0")
    parser.add_argument("--config-template", type=Path)
    parser.add_argument("--vita-pack-vpk", type=Path, default=shutil.which("vita-pack-vpk"))
    parser.add_argument("--vita-mksfoex", type=Path, default=shutil.which("vita-mksfoex"))
    parser.add_argument("--smoke-exe", type=Path, default=ROOT / "build/vita-system12/smoke.exe")
    parser.add_argument("--profile", type=Path, default=PROFILE)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--timeout", type=float,
                        help="per-process timeout (replay defaults to 1200 seconds)")
    parser.add_argument("--gameplay-replay", action="store_true",
                        help="replay deterministic inputs through the selected replay frame count")
    parser.add_argument("--replay-frames", type=int, choices=GAMEPLAY_REPLAY_FRAME_CHOICES,
                        help="replay inputs through frame 3,600, 6,000, or 12,000 (requires --gameplay-replay)")
    parser.add_argument("--frames", type=int, choices=DIAGNOSTIC_MILESTONES,
                        help="run only this frame milestone (400, 800, 1800, or 3600)")
    parser.add_argument("--gpu-thread", action="store_true",
                        help="pass --gpu-thread to the guest (VITA_GPU_THREAD=1 NEON VPK)")
    parser.add_argument("--reuse-console-smoke", type=Path,
                        help="reuse smoke evidence JSON when VPK and host hashes match")
    args = parser.parse_args()
    try:
        frames_to_run = select_milestones(args.frames, args.gameplay_replay, args.replay_frames)
    except ValueError as error:
        parser.error(str(error))
    if args.timeout is None:
        args.timeout = 1200 if args.gameplay_replay else 240
    if (not (args.emulator.is_file() or args.emulator.is_dir()) or not args.vpk.is_file() or
            not args.firmware_fs.is_dir() or
            (not args.reuse_console_smoke and not args.smoke_exe.is_file()) or
            not args.profile.is_dir() or args.timeout <= 0 or
            (args.renderer == "vitagl" and (not args.shader_compiler or
                                           not args.shader_compiler.is_file())) or
            not args.vita_pack_vpk or
            not args.vita_mksfoex):
        parser.error("emulator, VPK, local firmware/profile, pack tools, and timeout are required")
    if args.output_dir.exists():
        parser.error("output directory already exists; choose a fresh path")
    vpk_hash = sha256_file(args.vpk.resolve())
    host_hash = sha256_file(emulator_executable(args.emulator.resolve()))
    reused_smoke = None
    if args.reuse_console_smoke:
        reused_smoke = validate_smoke_evidence(
            args.reuse_console_smoke, vpk_hash, host_hash)
    args.output_dir.mkdir(parents=True)

    # Reuse the repository's complete manifest/CRC/SHA1/source-commit audit.
    if args.profile.resolve() != PROFILE.resolve():
        raise RuntimeError("custom profile directories are not accepted by this validation runner")
    manifest = validate_profile()
    if manifest["profile"]["description"] != "Soul Calibur (Asia, SOC14/VER.C)":
        raise RuntimeError("unexpected game profile")

    with tempfile.TemporaryDirectory(prefix="pcsx-vita-system12-gameplay-") as temp:
        base = Path(temp)
        emulator, fs, portable = read_cli(base, args)
        if args.renderer == "vitagl":
            checker = load_module("vitagl_game_checker", ROOT / "tools/vita-harness/check_vita3k.py")
            checker.install_local_shader_compiler(emulator, args.shader_compiler)
        (args.output_dir / "native-screenshot-setup.json").write_text(json.dumps({
            "config_key": "screenshot-format", "config_value": 2,
            "auto_screenshot_env": (GAMEPLAY_SCREENSHOT_TIMES if args.gameplay_replay or args.frames == 3600
                                     else AUTO_SCREENSHOT_TIMES),
            "native_capture_root": "portable/screenshots/<Vita3K title>",
        }, indent=2) + "\n", encoding="utf-8")
        shutil.copy2(portable / "config.yml", args.output_dir / "vita3k-config.yml")
        game_files = [(args.profile / name, f"soulclbrac/{name}") for name in GAME_FILES]
        input_events = None
        inputs_path = None
        if args.gameplay_replay:
            inputs_path = args.output_dir / "inputs.csv"
            replay_frames = frames_to_run[0]
            input_events = write_gameplay_replay(inputs_path, replay_frames)
            (args.output_dir / "inputs.json").write_text(json.dumps(
                [{"frame": frame, "control": control, "pressed": bool(value)}
                 for frame, control, value in input_events], indent=2) + "\n",
                encoding="utf-8")
            game_files.append((inputs_path, "soulclbrac/inputs.csv"))
        game_vpk = make_vpk(args.vpk.resolve(), Path(args.vita_pack_vpk),
                            Path(args.vita_mksfoex), base / "soulclbrac.vpk", base,
                            TITLE_ID, "PCSX Vita System12 Soul Calibur", game_files)

        log = args.output_dir / "vita3k.log"
        log.touch()
        if reused_smoke is None:
            smoke_vpk = make_vpk(args.vpk.resolve(), Path(args.vita_pack_vpk),
                                 Path(args.vita_mksfoex), base / "console-smoke.vpk",
                                 base, SMOKE_ID, "PCSX Vita console smoke",
                                 [(args.smoke_exe, "smoke.exe")])
            if not install_vpk(emulator, smoke_vpk, SMOKE_ID, log, args.timeout):
                raise RuntimeError(f"console smoke VPK installation failed; log={log}")
            # The console smoke app is run under its own installed title.
            process = subprocess.Popen([str(emulator), "--keep-config", "--log-level", "1",
                "--installed-path", SMOKE_ID, "--app-args",
                f"pcsx-vita, ux0:app/{SMOKE_ID}/smoke.exe, --frames, 3, --verify-smoke"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
            smoke_watchdog = ProcessGroupWatchdog(process, args.timeout)
            console_tokens = (
                "pcsx-vita: smoke PASS cpu=ari64 triangle=03ff background=2082 "
                "sentinel=534d4f4b runs=3",)
            console_capture = CapturedGuest(process, log, "console smoke", console_tokens)
            console_natural_exit = False
            try:
                console_ok = console_capture.wait_for_messages(console_tokens, args.timeout)
            finally:
                console_natural_exit = process.poll() is not None
                smoke_watchdog.close()
                console_capture.close()
            console_ok = (console_ok and not any(console_capture.issue_counts.values()) and
                          not smoke_watchdog.timed_out and
                          process.returncode in (0, -signal.SIGTERM, -signal.SIGKILL) and
                          (not console_natural_exit or process.returncode == 0))
            if not console_ok:
                raise RuntimeError(f"console {args.renderer} smoke failed; log={log}")
            smoke_record = {"console_smoke": "PASS", "vpk_sha256": vpk_hash,
                            "host_sha256": host_hash, "reused": False}
        else:
            smoke_record = {"console_smoke": "PASS", "vpk_sha256": vpk_hash,
                            "host_sha256": host_hash, "reused": True,
                            "evidence": str(args.reuse_console_smoke.resolve())}
        (args.output_dir / "smoke-evidence.json").write_text(
            json.dumps(smoke_record, indent=2) + "\n", encoding="utf-8")

        if not install_vpk(emulator, game_vpk, TITLE_ID, log, args.timeout):
            raise RuntimeError(f"Soul Calibur VPK installation failed; log={log}")
        (args.output_dir / "profile-manifest.json").write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        results = []
        prior_h8_cycles = 0
        prior_pcm_frames = 0
        guest_data = fs / "ux0/data"
        screenshot_root = portable / "screenshots"
        results_metadata = {
            "renderer": args.renderer,
            "vpk": str(args.vpk.resolve()), "vpk_sha256": vpk_hash,
            "host_sha256": host_hash, "title_id": TITLE_ID,
            "console_smoke": "PASS", "board_execution_verified": False,
            "console_smoke_reused": bool(reused_smoke),
            "gameplay_replay": bool(inputs_path),
            "input_event_count": len(input_events) if input_events is not None else 0,
        }
        completed_milestones = []
        for frames in frames_to_run:
            report_guest = f"ux0:data/system12-{frames}.txt"
            capture_guest = f"ux0:data/system12-{frames}.ppm"
            args_text = (f"pcsx-vita, --system12, ux0:app/{TITLE_ID}/soulclbrac, "
                         f"--frames, {frames}, --report-system12, {report_guest}")
            if args.renderer == "neon":
                args_text += f", --capture-ppm, {capture_guest}"
            if inputs_path:
                args_text += f", --inputs, ux0:app/{TITLE_ID}/soulclbrac/inputs.csv"
            if args.gpu_thread:
                args_text += ", --gpu-thread"
            expected = (f"pcsx-vita: system12-report PASS path={report_guest}",
                        "pcsx-vita: cpu_driver=ari64")
            if args.renderer == "neon":
                expected += (f"pcsx-vita: system12-capture PASS path={capture_guest}",)
            else:
                expected += ("pcsx-vita: GPU = CTR High Octane native, VRAM=1024x1024",
                             "pcsx-vita: vitaGL shutdown PASS (0)")
            if inputs_path:
                event_count = len(input_events)
                expected += (f"pcsx-vita: inputs replay PASS events={event_count}/{event_count} "
                             f"frames={frames}/{frames} final=0002/0000",)
            before_screenshots = (set(screenshot_root.rglob("*.png"))
                                  if screenshot_root.exists() else set())
            guest_ok, runtime_issues = False, {}
            report_info = None
            capture_info = None
            error = None
            try:
                guest_ok, runtime_issues = run_guest(
                    emulator, args_text, log, args.timeout, expected,
                    extra_env={"VITA3K_AUTO_SCREENSHOT": (GAMEPLAY_SCREENSHOT_TIMES
                        if inputs_path or frames == 3600 else AUTO_SCREENSHOT_TIMES)})
                if not guest_ok:
                    raise RuntimeError(f"System12 guest run failed at {frames} frames; log={log}")
                report_path = guest_data / f"system12-{frames}.txt"
                capture_path = guest_data / f"system12-{frames}.ppm"
                if not report_path.is_file() or (args.renderer == "neon" and not capture_path.is_file()):
                    raise RuntimeError(f"Vita guest did not produce evidence at {frames} frames")
                report_out = args.output_dir / f"system12-{frames}.txt"
                capture_out = args.output_dir / f"system12-{frames}.ppm"
                shutil.copy2(report_path, report_out)
                report_info = read_report(report_out, require_replay=bool(inputs_path))
                if args.renderer == "neon":
                    shutil.copy2(capture_path, capture_out)
                    capture_info = check_capture(capture_out)
                h8_cycles = int(report_info["records"]["H8"]["cycles"])
                pcm_frames = int(report_info["records"]["C352"]["frames"])
                if h8_cycles <= prior_h8_cycles or pcm_frames < prior_pcm_frames:
                    raise RuntimeError(f"board device progress stalled by frame {frames}")
                prior_h8_cycles, prior_pcm_frames = h8_cycles, pcm_frames
            except Exception as caught:
                error = caught
            finally:
                try:
                    screenshots, screenshot_error = preserve_guest_capture(
                        screenshot_root, before_screenshots, guest_data, args.output_dir,
                        frames, guest_ok, runtime_issues)
                except Exception as caught:
                    screenshots, screenshot_error = [], None
                    if error is None:
                        error = caught
            if error is None and screenshot_error:
                error = RuntimeError(screenshot_error)

            milestone = {"frames": frames, "status": "completed" if error is None else "failed",
                         "report": (f"system12-{frames}.txt"
                                    if (args.output_dir / f"system12-{frames}.txt").is_file() else None),
                         "capture": (f"system12-{frames}.ppm"
                                     if args.renderer == "neon" and
                                     (args.output_dir / f"system12-{frames}.ppm").is_file() else None),
                         "capture_info": capture_info,
                         "native_screenshots": screenshots,
                         "native_screenshot_error": screenshot_error,
                         "report_info": report_info,
                         "runtime_issues": runtime_issues}
            if error is not None:
                milestone["error"] = f"{type(error).__name__}: {error}"
                results.append(milestone)
                failure = {"frames": frames, "error": milestone["error"],
                           "runtime_issues": runtime_issues}
                write_results(args.output_dir / "results.json", results_metadata,
                              results, completed_milestones, "failed", failure)
                raise error

            results.append(milestone)
            completed_milestones.append(frames)
            write_results(args.output_dir / "results.json", results_metadata,
                          results, completed_milestones, "running")

        if (not inputs_path and
                len({r["report_info"]["records"]["H8"]["cycles"] for r in results}) < len(frames_to_run)):
            error = RuntimeError("H8 cycles did not change across all requested milestones")
            failure = {"frames": list(frames_to_run), "error": f"{type(error).__name__}: {error}"}
            write_results(args.output_dir / "results.json", results_metadata,
                          results, completed_milestones, "failed", failure)
            raise error

        has_runtime_issues = any(result["runtime_issues"] for result in results)
        write_results(args.output_dir / "results.json", results_metadata, results,
                      completed_milestones, "failed" if has_runtime_issues else "passed")
    print(f"Vita3K System12 gameplay evidence captured: {args.output_dir}")
    return 1 if has_runtime_issues else 0


if __name__ == "__main__":
    raise SystemExit(main())
