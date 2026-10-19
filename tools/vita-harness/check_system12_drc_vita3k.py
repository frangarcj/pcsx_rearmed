#!/usr/bin/env python3
"""Run synthetic 4/16 MiB System12 DRC probes in an isolated Vita3K profile."""
import argparse
import ctypes
import errno
from functools import lru_cache
import importlib.util
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = Path(__file__).resolve().parents[2]
TITLE_ID = "S12DRC001"
FIXTURE_MODULE = ROOT / "tests" / "check_system12_drc.py"
RUNNER_MODULE = ROOT / "tools" / "vita-harness" / "check_vita3k.py"
EXPECTED = re.compile(
    r"pcsx-vita: system12-drc PASS cpu=ari64 ram=(4|16)M "
    r"magic=53313244 bank0=00001111 bank1=00002222 "
    r"upper=51a7cafe alias=(?:00001357|00002468) "
    r"smc=00001234/00005678 dma5=00003333 mmio=76543210 "
    r"half=00001357 byte=00000089 runs=1\b")
_CHECKER = None


@lru_cache(maxsize=1)
def _native_clonefile():
    if sys.platform != "darwin":
        return None
    try:
        clone = ctypes.CDLL(None, use_errno=True).clonefile
    except AttributeError:
        return None
    clone.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int)
    clone.restype = ctypes.c_int
    return clone


def copy_runtime_file(source, destination, *, follow_symlinks=True):
    """Independent APFS clone for private runtimes; ordinary copy elsewhere.

    A clone has its own inode and permissions. Guest writes and the firmware
    read-only chmod cannot affect the source, unlike hard links. Preserve
    ENOSPC/permission errors instead of retrying them as a full data copy.
    """
    source, destination = Path(source), Path(destination)
    if destination.is_dir():
        destination = destination / source.name
    clone = _native_clonefile() if follow_symlinks and source.is_file() else None
    if clone is not None and not os.path.lexists(destination):
        if clone(os.fsencode(source.resolve()), os.fsencode(destination), 0) == 0:
            shutil.copystat(source, destination, follow_symlinks=True)
            return str(destination)
        error = ctypes.get_errno()
        if error not in (errno.EXDEV, errno.ENOTSUP, errno.EINVAL, errno.ENOSYS):
            raise OSError(error, os.strerror(error), str(destination))
    return shutil.copy2(source, destination, follow_symlinks=follow_symlinks)


def system12_result_matches(text, ram_mb):
    match = EXPECTED.search(text)
    expected_alias = "00001357" if ram_mb == 4 else "00002468"
    return bool(match and int(match.group(1)) == ram_mb and
                f"alias={expected_alias}" in match.group(0))


def completed_guest_matches(text, ram_mb):
    return (system12_result_matches(text, ram_mb) and
            "Stopping -> Idle" in text)


def load_fixture_module():
    spec = importlib.util.spec_from_file_location("system12_drc_fixture", FIXTURE_MODULE)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load the shared System12 DRC fixture generator")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def vita3k_fatal(log):
    global _CHECKER
    if _CHECKER is None:
        _CHECKER = load_checker_module()
    checker = _CHECKER
    return checker.fatal(log) or "pcsx-vita: system12-drc FAIL" in log


def load_checker_module():
    spec = importlib.util.spec_from_file_location("vita3k_checker", RUNNER_MODULE)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load the Vita3K process checker")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_until(process, log_path, predicate, timeout, label):
    checker = load_checker_module()
    class CapturedDrc(checker.CapturedSmoke):
        ISSUE_MARKERS = checker.CapturedSmoke.ISSUE_MARKERS + (b"pcsx-vita: system12-drc FAIL",)

    watchdog = checker.ProcessGroupWatchdog(process, timeout)
    captured = CapturedDrc(process, log_path, label)
    deadline = time.monotonic() + timeout
    passed_at = None
    passed = False
    natural_exit = False
    try:
        while time.monotonic() < deadline:
            if any(captured.issue_counts.values()):
                break
            log = "\n".join(captured.messages) + "\n" + "\n".join(captured.markers)
            if predicate(log):
                if passed_at is None:
                    passed_at = time.monotonic()
                if time.monotonic() - passed_at >= 0.75:
                    passed = process.poll() in (None, 0)
                    break
            if process.poll() is not None:
                captured.thread.join(timeout=1)
                log = "\n".join(captured.messages) + "\n" + "\n".join(captured.markers)
                passed = process.returncode == 0 and predicate(log)
                break
            time.sleep(0.05)
        else:
            print(f"Vita3K {label}: timed out; log={log_path}")
    finally:
        natural_exit = process.poll() is not None
        watchdog.close()
        captured.close()
    final_log = "\n".join(captured.messages) + "\n" + "\n".join(captured.markers)
    return (passed and not watchdog.timed_out and not any(captured.issue_counts.values()) and
            predicate(final_log) and process.returncode in (0, -signal.SIGTERM, -signal.SIGKILL) and
            (not natural_exit or process.returncode == 0))


def build_fixture_vpk(base_vpk, packer, mksfoex, temp_root):
    fixtures = []
    fixture_module = load_fixture_module()
    for ram_mb in (4, 16):
        fixture = temp_root / f"system12-{ram_mb}"
        fixture.mkdir()
        fixture_module.make_regions(fixture, ram_mb, vita_signature=True)
        fixtures.append((ram_mb, fixture))

    package_inputs = temp_root / f"package-inputs-{ram_mb}"
    package_inputs.mkdir()
    with zipfile.ZipFile(base_vpk) as archive:
        for name in ("sce_sys/param.sfo", "eboot.bin"):
            archive.extract(name, package_inputs)

    sfo_path = package_inputs / "sce_sys/param.sfo"
    subprocess.run([str(mksfoex), "-s", f"TITLE_ID={TITLE_ID}",
                    "-d", "ATTRIBUTE2=12", "PCSX System12 DRC fixture",
                    str(sfo_path)], check=True, capture_output=True,
                   text=True, timeout=30)
    output_vpk = temp_root / "system12-drc-fixtures.vpk"
    command = [str(packer), "-s", str(sfo_path),
               "-b", str(package_inputs / "eboot.bin")]
    for ram_mb, fixture in fixtures:
        for name in ("main.bin", "banked.bin", "sub.bin", "wave.bin", "board.cfg"):
            path = fixture / name
            if path.exists():
                command += ["-a", f"{path}=system12-{ram_mb}/{name}"]
    command.append(str(output_vpk))
    subprocess.run(command, check=True, capture_output=True, text=True, timeout=120)
    return output_vpk


def make_isolated_config(config_path, fs_path, firmware_fs, config_template):
    fs_path.mkdir(parents=True)
    for name in ("vs0", "os0", "sa0"):
        source = firmware_fs / name
        if not source.is_dir():
            raise RuntimeError(f"firmware tree is missing {name}: {source}")
        copied = fs_path / name
        shutil.copytree(source, copied, copy_function=copy_runtime_file)
        for path in (copied, *copied.rglob("*")):
            mode = path.stat().st_mode
            if not path.is_dir():
                os.chmod(path, mode & ~(stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH))
    for name in ("ux0", "ur0", "grw0", "host0", "imc0", "ur0/data"):
        (fs_path / name).mkdir(parents=True, exist_ok=True)
    config, replacements = re.subn(
        r"(?m)^pref-path:.*$", f"pref-path: {fs_path.resolve()}/",
        config_template.read_text(encoding="utf-8"))
    if replacements != 1:
        raise RuntimeError("Vita3K config template must have exactly one pref-path key")
    config_path.write_text(config, encoding="utf-8")


def locate_app_bundle(emulator):
    emulator = emulator.absolute()
    if emulator.suffix == ".app" and emulator.is_dir():
        bundle = emulator
        binary = bundle / "Contents" / "MacOS" / "Vita3K"
    else:
        binary = emulator
        bundle = next((parent for parent in binary.parents
                       if parent.suffix == ".app"), None)
    if bundle is None or not bundle.is_dir() or not binary.is_file():
        raise RuntimeError("emulator must be inside a Vita3K.app bundle, or name that bundle")
    try:
        binary.resolve().relative_to(bundle.resolve())
    except ValueError as error:
        raise RuntimeError("Vita3K executable resolves outside its app bundle") from error
    return bundle, binary.relative_to(bundle)


def run_profile(emulator, log_path, timeout, ram_mb):
    base = [str(emulator), "--keep-config", "--log-level", "1"]
    app_args = (f"pcsx-vita, --system12, ux0:app/{TITLE_ID}/system12-{ram_mb}, "
                f"--verify-system12-drc, {ram_mb}, --frames, 1")
    guest = subprocess.Popen(base + ["--installed-path", TITLE_ID,
                                     "--app-args", app_args], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, start_new_session=True)
    return run_until(guest, log_path, lambda text: completed_guest_matches(text, ram_mb),
                     timeout, f"System12 {ram_mb}M DRC")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", type=Path, required=True)
    parser.add_argument("--vpk", type=Path, required=True,
                        help="ARCADE_SYSTEM12=1 Vita NEON VPK built with the System12 adapter")
    parser.add_argument("--vita-pack-vpk", type=Path, default=shutil.which("vita-pack-vpk"))
    parser.add_argument("--vita-mksfoex", type=Path, default=shutil.which("vita-mksfoex"))
    parser.add_argument("--firmware-fs", type=Path, required=True,
                        help="Vita3K firmware fs root containing vs0/os0/sa0; copied to temp only")
    parser.add_argument("--config-template", type=Path,
                        help="Vita3K config.yml template; defaults to firmware-fs/../config.yml")
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--log", type=Path, required=True)
    args = parser.parse_args()
    config_template = args.config_template or args.firmware_fs.parent / "config.yml"
    emulator_ok = args.emulator.is_file() or (
        args.emulator.is_dir() and args.emulator.suffix == ".app")
    if (not 0 < args.timeout < float("inf") or not emulator_ok or not args.vpk.is_file()
            or not args.firmware_fs.is_dir() or not config_template.is_file()):
        parser.error("emulator, VPK, firmware fs, and a positive timeout are required")
    packer = args.vita_pack_vpk
    if not packer or not Path(packer).is_file():
        parser.error("vita-pack-vpk was not found; pass --vita-pack-vpk")
    mksfoex = args.vita_mksfoex
    if not mksfoex or not Path(mksfoex).is_file():
        parser.error("vita-mksfoex was not found; pass --vita-mksfoex")

    args.log.parent.mkdir(parents=True, exist_ok=True)
    if args.log.exists():
        parser.error("log path already exists; use a fresh output path")

    results = []
    with tempfile.TemporaryDirectory(prefix="pcsx-vita-system12-drc-") as tmp:
        root = Path(tmp)
        source_bundle, binary_relative = locate_app_bundle(args.emulator)
        isolated_bundle = root / source_bundle.name
        shutil.copytree(source_bundle, isolated_bundle, symlinks=True,
                        copy_function=copy_runtime_file)
        emulator = isolated_bundle / binary_relative
        portable = root / "portable"
        portable.mkdir()
        package = build_fixture_vpk(args.vpk.resolve(), Path(packer),
                                    Path(mksfoex), root)
        config_path = portable / "config.yml"
        fs_path = portable / "fs"
        make_isolated_config(config_path, fs_path, args.firmware_fs.resolve(),
                             config_template.resolve())
        install = subprocess.Popen([str(emulator), "--keep-config",
                                    "--log-level", "1", "--", str(package)],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        install_ok = run_until(
            install, args.log,
            lambda text: f"[{TITLE_ID}] installed successfully!" in text,
            args.timeout, "System12 fixture install")
        if not install_ok:
            print(f"Vita3K System12 fixture installation failed; log={args.log}")
            return 1
        for ram_mb in (4, 16):
            results.append(run_profile(emulator, args.log, args.timeout, ram_mb))
            if not results[-1]:
                break
    passed = len(results) == 2 and all(results)
    print(f"Vita3K System12 synthetic ARMv7 DRC: {'PASS' if passed else 'FAIL'}; log={args.log}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
