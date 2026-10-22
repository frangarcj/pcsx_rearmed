#!/usr/bin/env python3
"""Verify local System 10 MAME media and package supported base-board images.

No ROMs are downloaded. The output preserves each complete MAME ROM region;
it is an input package for the standalone MEM(M)/MEM(N) adapter, not a claim
that the game or every attached board is emulated.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Any, Iterable

import prepare_mame_psx as mame
import system10_boards as boards

DRIVER = boards.DRIVER
PROFILE_PATH = Path(__file__).with_name("system10_boards.json")


class System10PreparationError(Exception):
    """Source, profile, image, or output is unsupported or invalid."""


def load_profiles(path: Path = PROFILE_PATH) -> tuple[str, dict[str, dict[str, Any]]]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise System10PreparationError(f"cannot read System 10 profiles: {exc}") from exc
    rows = document.get("profiles")
    if (document.get("schema_version") != 1 or
            document.get("mame_source_commit") != mame.PINNED_MAME_COMMIT or
            document.get("source") != DRIVER or document.get("set_count") != 45 or
            not isinstance(rows, list) or len(rows) != 45):
        raise System10PreparationError("profile registry must contain all 45 sets from pinned MAME")
    profiles: dict[str, dict[str, Any]] = {}
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get("set"), str):
            raise System10PreparationError("invalid System 10 profile row")
        if row["set"] in profiles:
            raise System10PreparationError(f"duplicate System 10 profile: {row['set']}")
        profiles[row["set"]] = row
    return document["mame_source_commit"], profiles


def _source_commit(source: Path) -> str:
    try:
        return subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise System10PreparationError(f"cannot identify MAME checkout at {source}") from exc


def _verify_profile(set_name: str, profile: dict[str, Any], source_path: Path) -> None:
    try:
        source = source_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise System10PreparationError(f"cannot read System 10 source {source_path}: {exc}") from exc
    clean_source = mame._without_comments(source)
    rows = boards._game_rows(clean_source)
    if len(rows) != 45 or set(rows) != set(load_profiles()[1]):
        raise System10PreparationError("pinned source does not contain the complete 45-set registry")
    actual = rows.get(set_name)
    if actual is None:
        raise System10PreparationError(f"{set_name} is not a System 10 GAME entry")
    for key in ("year", "machine", "input", "state", "init", "flags", "description"):
        if profile.get(key) != actual.get(key):
            raise System10PreparationError(f"registry/source mismatch for {set_name}: {key}")
    if profile.get("parent") != actual.get("parent"):
        raise System10PreparationError(f"registry/source mismatch for {set_name}: parent")
    _, source_crypto = boards._machine_facts(clean_source, actual["machine"], profile["board_type"])
    if profile.get("crypto_profile") != source_crypto:
        raise System10PreparationError(f"registry/source mismatch for {set_name}: crypto profile")
    expected_source = (f"machine:{actual['machine']}" if profile["board_type"] == "MEM(N)" else
                       f"init:{actual['init']}" if profile["board_type"] == "MEM(M)" else None)
    if profile.get("crypto_profile_source") != expected_source:
        raise System10PreparationError(f"registry/source mismatch for {set_name}: crypto profile source")
    try:
        regenerated = boards.build_registry(source_path.parents[3])
    except (boards.System10ProfileError, mame.PreparationError) as exc:
        raise System10PreparationError(f"cannot reconstruct pinned source profile: {exc}") from exc
    generated_rows = {row["set"]: row for row in regenerated["profiles"]}
    if generated_rows.get(set_name) != profile:
        raise System10PreparationError(
            f"registry profile for {set_name} differs from the complete pinned source extraction")


def _refuse_unsupported(set_name: str, profile: dict[str, Any]) -> None:
    reasons = profile.get("unsupported_requirements")
    if not isinstance(reasons, list):
        raise System10PreparationError(f"{set_name} has no audited support requirements")
    if reasons:
        raise System10PreparationError(
            f"{set_name} requires unsupported hardware/media: " + "; ".join(reasons))
    if profile.get("board_type") not in ("MEM(M)", "MEM(N)"):
        raise System10PreparationError(f"{set_name} board type {profile.get('board_type')!r} is not implemented")
    if not profile.get("crypto_profile"):
        raise System10PreparationError(f"{set_name} has no source-derived native crypto profile")


def _load_regions(set_name: str, source: Path, rompath: Path,
                  archive: Path | None) -> tuple[dict[str, bytes], dict[str, Any]]:
    try:
        romset, parents, _ = mame._find_romset(source, set_name)
        media, origins = mame._load_rom_files(
            romset, rompath, mame._lineage(set_name, parents), None, archive)
        regions = mame.assemble_regions(romset, media)
        return regions, {
            "rom_set": romset.name,
            "rom_files": [{
                "filename": spec.filename, "size": spec.expected_size,
                "crc32": spec.crc32, "sha1": spec.sha1, "status": spec.status,
                "provided_by": origins.get(spec.index),
            } for spec in romset.files],
        }
    except mame.PreparationError as exc:
        raise System10PreparationError(str(exc)) from exc


def _digest(data: bytes) -> tuple[str, str]:
    import zlib
    return f"{zlib.crc32(data) & 0xffffffff:08x}", hashlib.sha1(data).hexdigest()


def _make_payloads(profile: dict[str, Any], regions: dict[str, bytes]) -> tuple[dict[str, bytes], dict[str, str]]:
    board_type = profile["board_type"]
    expected_sizes = {region["tag"]: region["size"] for region in profile.get("rom_regions", [])}
    if set(regions) != set(expected_sizes):
        missing = set(expected_sizes) - set(regions)
        unexpected = set(regions) - set(expected_sizes)
        details = []
        if missing:
            details.append("missing " + ", ".join(sorted(missing)))
        if unexpected:
            details.append("unsupported " + ", ".join(sorted(unexpected)))
        raise System10PreparationError("assembled region table differs from source profile: " + "; ".join(details))
    wrong_size = [f"{tag}={len(regions[tag])} (expected {size})"
                  for tag, size in expected_sizes.items() if len(regions[tag]) != size]
    if wrong_size:
        raise System10PreparationError("assembled region size differs from source profile: " + ", ".join(wrong_size))
    if board_type == "MEM(M)":
        required = {"maincpu:rom", "nand", "data"}
        names = {"maincpu:rom": "main.bin", "nand": "rawflash.bin", "data": "datarom.bin"}
    elif board_type == "MEM(N)":
        nand_tags = sorted((tag for tag in regions if re.fullmatch(r"nand\d+", tag)),
                           key=lambda tag: int(tag[4:]))
        required = {"maincpu:rom", "nand0"}
        required.update(nand_tags)
        names = {"maincpu:rom": "main.bin", **{tag: f"raw{tag}.bin" for tag in nand_tags}}
    else:
        raise System10PreparationError(f"unsupported board kind {board_type}")
    missing = required - set(regions)
    unexpected = set(regions) - required
    if missing:
        raise System10PreparationError(f"required assembled regions are missing: {', '.join(sorted(missing))}")
    if unexpected:
        raise System10PreparationError(f"additional ROM regions are not supported: {', '.join(sorted(unexpected))}")
    return ({names[tag]: regions[tag] for tag in required}, names)


def board_ini(set_name: str, profile: dict[str, Any], region_names: dict[str, str],
              region_sizes: dict[str, int]) -> str:
    """The package's board.ini: region_names maps each region tag to its file."""
    lines = [
        "format=pcsx-aow-system10-v1", f"mame_set={set_name}",
        f"mame_parent={profile.get('parent') or 'none'}",
        f"board_type={profile['board_type']}",
        f"crypto_profile={profile['crypto_profile']}",
        f"mame_machine={profile['machine']}", f"mame_input={profile['input']}",
        f"mame_state={profile['state']}", f"mame_init={profile['init']}",
        f"mame_flags={profile['flags']}", "board_execution_verified=false",
    ]
    for tag, filename in region_names.items():
        lines.append(f"region.{tag}={filename}")
        lines.append(f"region.{tag}.size={region_sizes[tag]}")
    return "\n".join(lines) + "\n"


def region_files(profile: dict[str, Any], tags: Iterable[str]) -> dict[str, str]:
    """Package file of each assembled region of a MEM(M) or MEM(N) profile."""
    tags = set(tags)
    if profile["board_type"] == "MEM(M)":
        names = {"maincpu:rom": "main.bin", "nand": "rawflash.bin", "data": "datarom.bin"}
        return {tag: name for tag, name in names.items() if tag in tags}
    nand_tags = sorted((tag for tag in tags if re.fullmatch(r"nand\d+", tag)), key=lambda tag: int(tag[4:]))
    return {"maincpu:rom": "main.bin", **{tag: f"raw{tag}.bin" for tag in nand_tags}}


def package_regions(set_name: str, profile: dict[str, Any], regions: dict[str, bytes],
                    output_dir: Path, source_metadata: dict[str, Any] | None = None) -> dict[str, Any]:
    _refuse_unsupported(set_name, profile)
    payloads, region_names = _make_payloads(profile, regions)
    board_ini_text = board_ini(set_name, profile, region_names, {tag: len(regions[tag]) for tag in region_names})
    output_dir.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="prepare-system10-") as temp:
        stage = Path(temp)
        payload_info = {}
        for name, data in payloads.items():
            (stage / name).write_bytes(data)
            crc, sha = _digest(data)
            payload_info[name] = {"size": len(data), "crc32": crc, "sha1": sha}
        (stage / "board.ini").write_text(board_ini_text, encoding="ascii")
        manifest = {
            "system": "Namco System 10", "mame_driver": DRIVER,
            "mame_source_commit": mame.PINNED_MAME_COMMIT, "mame_set": set_name,
            "mame_parent": profile.get("parent"),
            "profile": {key: profile[key] for key in (
                "board_type", "machine", "input", "state", "init", "flags", "crypto_profile",
                "crypto_profile_source")},
            "regions": {tag: payload_info[filename] | {"file": filename}
                        for tag, filename in region_names.items()},
            "rom_source": source_metadata or {},
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
    if set_name not in profiles:
        raise System10PreparationError(f"no System 10 profile for {set_name}")
    profile = profiles[set_name]
    _refuse_unsupported(set_name, profile)
    actual_commit = _source_commit(source)
    if actual_commit != pinned_commit:
        raise System10PreparationError(f"MAME source commit {actual_commit} differs from pinned {pinned_commit}")
    _verify_profile(set_name, profile, source / "src/mame" / DRIVER)
    regions, metadata = _load_regions(set_name, source, rompath, archive)
    expected = {item["tag"]: item["size"] for item in profile["rom_regions"]
                if item["tag"] in ("maincpu:rom", "nand", "data") or re.fullmatch(r"nand\d+", item["tag"])}
    actual = {tag: len(data) for tag, data in regions.items()}
    if actual != expected:
        raise System10PreparationError(f"assembled region layout differs from pinned profile: {actual!r}")
    return package_regions(set_name, profile, regions, output_dir, metadata)


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--set", required=True, dest="set_name", help="MAME System 10 short name")
    parser.add_argument("--mame-source", required=True, type=Path, help="pinned local MAME checkout")
    parser.add_argument("--rompath", required=True, type=Path, help="directory of ROM ZIPs or loose files")
    parser.add_argument("--archive", type=Path, help="optional local ZIP searched first")
    parser.add_argument("--output-dir", type=Path, default=Path("artifacts/system10"))
    parser.add_argument("--profiles", type=Path, default=PROFILE_PATH)
    args = parser.parse_args(argv)
    try:
        result = prepare(args.set_name, args.mame_source, args.rompath,
                         args.output_dir, args.archive, args.profiles)
    except (OSError, System10PreparationError, zipfile.BadZipFile, mame.PreparationError) as exc:
        print(f"prepare_system10_board: {exc}", file=sys.stderr)
        return 2
    print(f"assembled {args.set_name}: {len(result['regions'])} regions; board execution remains unverified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
