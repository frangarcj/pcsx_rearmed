#!/usr/bin/env python3
"""Assemble verified local MAME System 11 media into the host board layout.

No ROMs are downloaded. This prepares ROM payload files and an INI profile;
it does not start a game or claim board execution is verified.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Any, Iterable

import prepare_mame_psx as mame

SCRIPT_DIR = Path(__file__).resolve().parent
PROFILE_PATH = SCRIPT_DIR / "system11_boards.json"
DRIVER = "namco/namcos11.cpp"
MCU_DRIVER = "namco/namcomcu.cpp"
EXPECTED_C76 = {
    "filename": "c76.bin",
    "size": 0x4000,
    "crc32": "399faac7",
    "sha1": "ceb184ef0486caf715dd997101999785f67a40b8",
}


class System11Error(Exception):
    """A profile, MAME source, archive, or payload is incomplete or unsupported."""


def load_profiles(path: Path = PROFILE_PATH) -> tuple[str, dict[str, dict[str, Any]]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise System11Error(f"cannot load System 11 profiles: {exc}") from exc
    commit = document.get("mame_source_commit")
    rows = document.get("profiles")
    if not isinstance(commit, str) or not isinstance(rows, list) or len(rows) != 36:
        raise System11Error("profile data must contain the pinned commit and all 36 GAME entries")
    profiles: dict[str, dict[str, Any]] = {}
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get("set"), str):
            raise System11Error("invalid System 11 profile entry")
        name = row["set"]
        if name in profiles:
            raise System11Error(f"duplicate System 11 profile: {name}")
        _validate_profile(name, row)
        profiles[name] = row
    return commit, profiles


def _validate_profile(name: str, profile: dict[str, Any]) -> None:
    if profile.get("model") not in ("COH100", "COH110"):
        raise System11Error(f"invalid CPU/GPU board for {name}")
    if profile.get("banking") not in ("NONE", "ROM8", "ROM8_64"):
        raise System11Error(f"invalid banking mode for {name}")
    if profile.get("keycus") not in {"C406", "C409", "C410", "C411", "C430", "C431", "C432", "C442", "C443"}:
        raise System11Error(f"invalid KEYCUS for {name}")
    if not isinstance(profile.get("lightgun"), bool):
        raise System11Error(f"missing lightgun flag for {name}")


def _game_rows(text: str) -> dict[str, dict[str, str]]:
    clean = mame._without_comments(text)
    rows: dict[str, dict[str, str]] = {}
    for match in re.finditer(r"\bGAME\s*\(", clean):
        args_text, _ = mame._call_arguments(clean, match.end() - 1)
        args = mame._split_arguments(args_text)
        if len(args) < 11:
            raise System11Error("unrecognized GAME declaration in namcos11.cpp")
        name = args[1].strip()
        if name in rows:
            raise System11Error(f"duplicate GAME declaration: {name}")
        rows[name] = {
            "year": args[0].strip(),
            "parent": args[2].strip(),
            "machine": args[3].strip(),
            "input": args[4].strip(),
            "flags": args[-1].strip(),
        }
    return rows


def _verify_profile_against_source(set_name: str, profile: dict[str, Any],
                                   driver_source: Path) -> None:
    try:
        rows = _game_rows(driver_source.read_text(encoding="utf-8"))
    except OSError as exc:
        raise System11Error(f"cannot read System 11 driver: {exc}") from exc
    if len(rows) != 36:
        raise System11Error(f"expected 36 GAME entries in namcos11.cpp, found {len(rows)}")
    actual = rows.get(set_name)
    if actual is None:
        raise System11Error(f"{set_name} is not a System 11 GAME entry")
    for field in ("year", "machine", "input", "flags"):
        if str(profile[field]) != actual[field]:
            raise System11Error(f"profile mismatch for {set_name}: {field} is {actual[field]!r} in MAME")
    actual_parent = None if actual["parent"] in ("0", "nullptr") else actual["parent"]
    if profile.get("parent") != actual_parent:
        raise System11Error(f"profile mismatch for {set_name}: parent is {actual_parent!r} in MAME")


def _source_commit(source: Path) -> str:
    try:
        return subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise System11Error(f"cannot identify MAME checkout at {source}") from exc


def _load_game_regions(set_name: str, source: Path, rompath: Path,
                       archive: Path | None) -> tuple[dict[str, bytes], dict[str, Any]]:
    try:
        romset, parents, _ = mame._find_romset(source, set_name)
        if romset.disks:
            names = ", ".join(disk.name for disk in romset.disks)
            raise System11Error(f"CHD/disk media is required and this loader does not assemble it: {names}")
        for required in ("maincpu:rom", "c76", "c352"):
            if required not in romset.regions:
                raise System11Error(f"{set_name} is missing required MAME region {required!r}")
        media, origins = mame._load_rom_files(
            romset, rompath, mame._lineage(set_name, parents), None, archive)
        regions = mame.assemble_regions(romset, media)
        return regions, {
            "rom_set": romset.name,
            "rom_files": [
                {"filename": spec.filename, "size": spec.expected_size,
                 "crc32": spec.crc32, "sha1": spec.sha1,
                 "provided_by": origins.get(spec.index)}
                for spec in romset.files if spec.index in media
            ],
        }
    except mame.PreparationError as exc:
        raise System11Error(str(exc)) from exc


def _load_c76_bios(source: Path, rompath: Path) -> tuple[bytes, str]:
    source_path = source / "src/mame" / MCU_DRIVER
    try:
        text = source_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise System11Error(f"cannot read C76 BIOS source {source_path}: {exc}") from exc
    # M37710_INTERNAL_ROM_REGION expands to the literal region name `internal`.
    text = text.replace("M37710_INTERNAL_ROM_REGION", '"internal"')
    sets, _, coverage = mame.parse_driver(text, MCU_DRIVER)
    if "c76" not in sets:
        failure = next((row["reason"] for row in coverage if row["set"] == "c76"), "not found")
        raise System11Error(f"cannot parse upstream C76 BIOS declaration: {failure}")
    romset = sets["c76"]
    if len(romset.files) != 1:
        raise System11Error("upstream C76 BIOS must declare exactly one ROM file")
    spec = romset.files[0]
    if (spec.filename != EXPECTED_C76["filename"] or spec.expected_size != EXPECTED_C76["size"]
            or spec.crc32 != EXPECTED_C76["crc32"] or spec.sha1 != EXPECTED_C76["sha1"]):
        raise System11Error("C76 BIOS declaration differs from pinned size/CRC/SHA1")
    try:
        media, origins = mame._load_rom_files(romset, rompath, [], None)
        regions = mame.assemble_regions(romset, media)
    except mame.PreparationError as exc:
        raise System11Error(f"C76 internal BIOS required: {exc}") from exc
    return regions["internal"][:EXPECTED_C76["size"]], origins[spec.index]


def _digest(data: bytes) -> tuple[str, str]:
    import zlib
    return f"{zlib.crc32(data) & 0xffffffff:08x}", hashlib.sha1(data).hexdigest()


def board_ini(set_name: str, profile: dict[str, Any]) -> str:
    return "\n".join([
        "format=pcsx-aow-system11-v1",
        f"mame_set={set_name}",
        f"mame_parent={profile.get('parent') or 'none'}",
        f"board_model={profile['model']}",
        f"banking={profile['banking']}",
        f"keycus={profile['keycus']}",
        f"lightgun={'true' if profile['lightgun'] else 'false'}",
        f"mame_machine={profile['machine']}",
        f"mame_input={profile['input']}",
        f"mame_flags={profile['flags']}",
        "board_execution_verified=false",
        "",
    ])


def package_regions(set_name: str, profile: dict[str, Any], regions: dict[str, bytes],
                    c76_bios: bytes, output_dir: Path,
                    source_metadata: dict[str, Any] | None = None,
                    c76_bios_origin: str | None = None) -> dict[str, Any]:
    _validate_profile(set_name, profile)
    if profile.get("flags") == "MACHINE_NOT_WORKING" or profile.get("status") == "not_working":
        raise System11Error(f"{set_name} is marked MACHINE_NOT_WORKING by upstream MAME")
    for tag in ("maincpu:rom", "c76", "c352"):
        if tag not in regions:
            raise System11Error(f"required assembled region is missing: {tag}")
    if profile.get("banking") != "NONE" and "bankedroms" not in regions:
        raise System11Error(f"{set_name} selects {profile['banking']} but has no bankedroms region")
    if len(c76_bios) != EXPECTED_C76["size"]:
        raise System11Error(f"C76 BIOS must be exactly {EXPECTED_C76['size']} bytes, got {len(c76_bios)}")
    payloads = {
        "main.bin": regions["maincpu:rom"],
        "banked.bin": regions.get("bankedroms", b""),
        "c76.bin": regions["c76"],
        "c76bios.bin": c76_bios,
        "wave.bin": regions["c352"],
    }
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="prepare-system11-") as temp_dir:
        stage = Path(temp_dir)
        payload_info: dict[str, dict[str, Any]] = {}
        for name, data in payloads.items():
            (stage / name).write_bytes(data)
            crc, sha = _digest(data)
            payload_info[name] = {"size": len(data), "crc32": crc, "sha1": sha}
        (stage / "board.ini").write_text(board_ini(set_name, profile), encoding="utf-8")
        manifest = {
            "mame_source_commit": load_profiles()[0],
            "mame_driver": DRIVER,
            "mame_set": set_name,
            "profile": {key: profile[key] for key in (
                "model", "banking", "keycus", "lightgun", "machine", "input", "flags")},
            "regions": payload_info,
            "rom_source": source_metadata or {},
            "c76_bios": {
                "source_driver": MCU_DRIVER,
                "filename": EXPECTED_C76["filename"],
                "size": EXPECTED_C76["size"],
                "crc32": EXPECTED_C76["crc32"],
                "sha1": EXPECTED_C76["sha1"],
                "provided_by": c76_bios_origin,
            },
            "pending_disk_media": [],
            "board_execution_verified": False,
        }
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        output_dir.mkdir(parents=True, exist_ok=True)
        for staged_file in stage.iterdir():
            staged_file.replace(output_dir / staged_file.name)
    return manifest


def prepare(set_name: str, source: Path, rompath: Path, output_dir: Path,
            archive: Path | None = None, profile_path: Path = PROFILE_PATH) -> dict[str, Any]:
    pinned_commit, profiles = load_profiles(profile_path)
    actual_commit = _source_commit(source)
    if actual_commit != pinned_commit:
        raise System11Error(f"MAME source commit {actual_commit} differs from pinned {pinned_commit}")
    if set_name not in profiles:
        raise System11Error(f"no System 11 profile for {set_name}")
    profile = profiles[set_name]
    driver_path = source / "src/mame" / DRIVER
    _verify_profile_against_source(set_name, profile, driver_path)
    if profile.get("status") == "not_working":
        raise System11Error(f"{set_name} is explicitly MACHINE_NOT_WORKING in upstream MAME")
    regions, metadata = _load_game_regions(set_name, source, rompath, archive)
    c76_bios, bios_origin = _load_c76_bios(source, rompath)
    return package_regions(set_name, profile, regions, c76_bios, output_dir,
                           metadata, bios_origin)


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--set", required=True, dest="set_name", help="MAME System 11 short name")
    parser.add_argument("--mame-source", required=True, type=Path, help="pinned local MAME checkout")
    parser.add_argument("--rompath", required=True, type=Path, help="local ROM ZIPs or loose ROM files")
    parser.add_argument("--archive", type=Path, help="optional local ZIP searched first")
    parser.add_argument("--output-dir", type=Path, default=Path("artifacts/system11"))
    args = parser.parse_args(argv)
    try:
        manifest = prepare(args.set_name, args.mame_source, args.rompath,
                           args.output_dir, args.archive)
    except (OSError, System11Error, zipfile.BadZipFile, mame.PreparationError) as exc:
        print(f"prepare_system11_board: {exc}", file=sys.stderr)
        return 2
    print(f"assembled {args.set_name}: {len(manifest['regions'])} payloads; board execution remains unverified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
