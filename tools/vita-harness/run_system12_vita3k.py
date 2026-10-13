#!/usr/bin/env python3
"""Run prepared System 10/11/12 or Capcom ZN board profiles in an isolated Vita3K and keep
its report, final-frame capture and Vita3K's own screenshots.

A lighter companion of check_system12_gameplay_vita3k.py (which validates
Soul Calibur end to end): it reuses that runner's isolation, packaging and
guest helpers, skips the console smoke, and accepts any profile directory
from prepare_system12_board.py.

  run_system12_vita3k.py --emulator Vita3K-fixed.app --vpk pcsx-rearmed-vita-neon.vpk \\
      --firmware-fs .../portable/fs --profile DIR --frames 6000 --output-dir OUT
"""
from __future__ import annotations

import argparse
import csv
import io
import importlib.util
import json
import hashlib
import re
from pathlib import Path
import shutil
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / "tools/vita-harness/check_system12_gameplay_vita3k.py"
CHECKER = ROOT / "tools/vita-harness/check_vita3k.py"
GAME_FILES = ("board.cfg", "main.bin", "banked.bin", "sub.bin", "wave.bin")


def canonical_replay(path: Path, frames: int) -> str:
    """Preserve the final per-frame button states, in Vita's strict order.

    Mac replays may order simultaneous independent controls differently.
    All events at one frame are applied before execution, so only the last
    value for a given control matters. Clip a longer replay to this run.
    """
    data = path.read_bytes()
    if len(data) > 1024 * 1024 or b"\0" in data:
        raise ValueError("invalid replay size or embedded NUL")
    events = {}
    previous = -1
    count = 0
    for row in csv.reader(data.decode("utf-8").splitlines()):
        row = [field.strip() for field in row]
        if not row or not row[0] or row[0].startswith("#"):
            continue
        if row == ["frame", "control", "pressed"]:
            if count:
                raise ValueError("replay header after events")
            continue
        if len(row) != 3 or not re.fullmatch(r"[0-9]+", row[0]) or row[2] not in ("0", "1"):
            raise ValueError("expected frame,control,pressed replay rows")
        frame, control, pressed = int(row[0]), row[1], int(row[2])
        if not re.fullmatch(r"p[12](up|down|left|right|button[1-8]|start)|coin[12]|service|test", control):
            raise ValueError(f"unknown replay control: {control}")
        if frame < previous or frame >= 216000:
            raise ValueError("replay frames must be ordered and below 216000")
        previous = frame
        count += 1
        if count > 4096:
            raise ValueError("replay exceeds 4096 events")
        if frame < frames:
            events[(frame, control)] = pressed
    if not events:
        raise ValueError("replay has no events within this run")
    output = io.StringIO()
    writer = csv.writer(output, lineterminator="\n")
    writer.writerow(("frame", "control", "pressed"))
    writer.writerows((frame, control, pressed)
                     for (frame, control), pressed in sorted(events.items()))
    return output.getvalue()


def load(path: Path):
    spec = importlib.util.spec_from_file_location("system12_vita3k_harness", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--emulator", type=Path, required=True)
    parser.add_argument("--vpk", type=Path, required=True)
    parser.add_argument("--firmware-fs", type=Path, required=True)
    parser.add_argument("--config-template", type=Path)
    parser.add_argument("--profile", type=Path, required=True, help="prepared arcade regions with board.cfg or board.ini")
    parser.add_argument("--frames", type=int, default=6000)
    parser.add_argument("--screenshots", default="5,10,20,30,45,60,75,90,105,120,150,180")
    parser.add_argument("--timeout", type=float, default=900)
    parser.add_argument("--gpu-thread", action="store_true")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--app-arg", action="append", default=[],
                        help="extra guest argument (repeat for each word, e.g. --app-arg=--gl-1x)")
    parser.add_argument("--inputs", type=Path,
                        help="frame,control,pressed CSV replayed by the guest (e.g. from tests/check_system12_gameplay.py)")
    parser.add_argument("--shader-compiler", type=Path,
                        help="local libshacccg.suprx; a vitaGL VPK compiles its shaders with it")
    parser.add_argument("--vita-pack-vpk", type=Path, default=shutil.which("vita-pack-vpk"))
    parser.add_argument("--vita-mksfoex", type=Path, default=shutil.which("vita-mksfoex"))
    args = parser.parse_args()
    if args.output_dir.exists():
        parser.error("output directory already exists; choose a fresh path")
    config_name = "board.ini" if (args.profile / "board.ini").is_file() else "board.cfg"
    cfg = dict(line.split("=", 1) for line in
               (args.profile / config_name).read_text().splitlines() if "=" in line)
    game_files = list(GAME_FILES)
    other_arcade = config_name == "board.ini"
    if other_arcade:
        if cfg.get("format") not in ("pcsx-aow-system10-v1", "pcsx-aow-system11-v1",
                                     "pcsx-aow-capcom-zn-v1"):
            parser.error("unsupported prepared arcade format")
        manifest = json.loads((args.profile / "manifest.json").read_text())
        if manifest["mame_set"] != cfg["mame_set"]:
            parser.error("profile/manifest set mismatch")
        game_files = [config_name]
        for key, region in manifest["regions"].items():
            filename = region.get("file", key)
            if Path(filename).name != filename:
                parser.error("region file must be a local basename")
            data = (args.profile / filename).read_bytes()
            if len(data) != region["size"] or hashlib.sha1(data).hexdigest() != region["sha1"]:
                parser.error(f"region hash mismatch: {filename}")
            game_files.append(filename)
    name = cfg["mame_set"]
    if cfg.get("unsupported", "none") != "none":
        parser.error(f"{name} declares unsupported devices: {cfg['unsupported']}")
    h = load(HARNESS)
    args.output_dir.mkdir(parents=True)
    replay = None
    if args.inputs:
        replay = args.output_dir / "inputs.csv"
        try:
            replay.write_text(canonical_replay(args.inputs, args.frames))
        except (ValueError, UnicodeError) as error:
            parser.error(str(error))
    with tempfile.TemporaryDirectory(prefix="pcsx-vita-system12-run-") as temp:
        base = Path(temp)
        emulator, fs, portable = h.read_cli(base, args)
        if args.shader_compiler:
            load(CHECKER).install_local_shader_compiler(emulator, args.shader_compiler)
            # vitaGL samples textures it rendered into (VRAM is a render
            # target); without surface sync Vita3K reads them as stale memory
            # and every textured polygon disappears.
            config = portable / "config.yml"
            text = config.read_text()
            if "\ndisable-surface-sync:" not in "\n" + text:
                raise SystemExit("Vita3K config has no disable-surface-sync key")
            config.write_text("\n".join("disable-surface-sync: false"
                                         if line.startswith("disable-surface-sync:") else line
                                         for line in text.splitlines()) + "\n")
        files = [(args.profile / f, f"{name}/{f}") for f in game_files]
        if replay:
            files.append((replay, f"{name}/inputs.csv"))
        vpk = h.make_vpk(args.vpk.resolve(), args.vita_pack_vpk, args.vita_mksfoex,
                         base / f"{name}.vpk", base, h.TITLE_ID, f"PCSX Vita System12 {name}", files)
        log = args.output_dir / "vita3k.log"
        log.touch()
        if not h.install_vpk(emulator, vpk, h.TITLE_ID, log, args.timeout):
            raise SystemExit(f"VPK installation failed; log={log}")
        report_guest = f"ux0:data/system12-{args.frames}.txt"
        capture_guest = f"ux0:data/system12-{args.frames}.ppm"
        app_args = (f"pcsx-vita, {'--arcade' if other_arcade else '--system12'}, ux0:app/{h.TITLE_ID}/{name}, --frames, {args.frames}, "
                    f"--report-system12, {report_guest}")
        if not args.shader_compiler:
            app_args += f", --capture-ppm, {capture_guest}"
        if args.gpu_thread:
            app_args += ", --gpu-thread"
        if args.inputs:
            app_args += f", --inputs, ux0:app/{h.TITLE_ID}/{name}/inputs.csv"
        for extra in args.app_arg:
            app_args += f", {extra}"
        expected = (f"pcsx-vita: system12-report PASS path={report_guest}",)
        if not args.shader_compiler:
            expected += (f"pcsx-vita: system12-capture PASS path={capture_guest}",)
        if args.shader_compiler:
            expected += ("pcsx-vita: vitaGL shutdown PASS (0)",)
        screenshot_root = portable / "screenshots"
        before = set(screenshot_root.rglob("*.png")) if screenshot_root.exists() else set()
        ok, issues = h.run_guest(emulator, app_args, log, args.timeout, expected,
                                 extra_env={"VITA3K_AUTO_SCREENSHOT": args.screenshots})
        shots = []
        if screenshot_root.exists():
            dest = args.output_dir / "screenshots"
            dest.mkdir()
            for png in sorted(set(screenshot_root.rglob("*.png")) - before):
                shutil.copy2(png, dest / png.name)
                shots.append(png.name)
        guest = fs / "ux0/data"
        for suffix in ("txt", "ppm"):
            src = guest / f"system12-{args.frames}.{suffix}"
            if src.is_file():
                shutil.copy2(src, args.output_dir / src.name)
        if other_arcade:
            report_path = args.output_dir / f"system12-{args.frames}.txt"
            report = report_path.read_text() if report_path.is_file() else ""
            text = log.read_text()
            if f"profile={name}" not in report or re.search(r"\b(?:faults?|illegal|unsupported|read|write|fetch)=[1-9]", report):
                ok = False; issues["Arcade board report failed"] = 1
            audio = re.search(r"arcade-audio rate=(\d+) frames=(\d+) peak=(\d+) nonzero=(\d+) errors=(\d+)", text)
            if not audio or not int(audio[3]) or int(audio[5]):
                ok = False; issues["Arcade audio failed"] = 1
            if not shots or (not args.shader_compiler and not (args.output_dir / f"system12-{args.frames}.ppm").is_file()):
                ok = False; issues["Arcade captures missing"] = 1
            if args.inputs and "inputs replay PASS" not in text:
                ok = False; issues["Arcade input replay failed"] = 1
        result = {"set": name, "frames": args.frames, "passed": ok, "issues": issues,
                  "screenshots": shots, "vpk": str(args.vpk.resolve()),
                  "vpk_sha256": hashlib.sha256(args.vpk.read_bytes()).hexdigest(),
                  "host_sha256": hashlib.sha256(h.emulator_executable(args.emulator).read_bytes()).hexdigest(),
                  "format": cfg.get("format", "system12"),
                  "cpu": "interpreter" if other_arcade else "ari64",
                  "inputs_sha256": hashlib.sha256(replay.read_bytes()).hexdigest() if replay else None,
                  "audio": list(map(int, audio.groups())) if other_arcade and audio else None}
        (args.output_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result, indent=2))
        raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
