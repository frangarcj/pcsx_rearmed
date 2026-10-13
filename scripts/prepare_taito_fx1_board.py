#!/usr/bin/env python3
"""Verify local Taito FX-1S ROMs and assemble raw board regions.

The output is a media package for the FX-1S adapter. It does not claim that
the complete board, sound path, or a game boots.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Iterable

import prepare_mame_psx as mame
import taito_fx1_boards as profiles

OUTPUTS = {
    "maincpu:rom": "main.bin",
    "bankedroms": "banked.bin",
    "audiocpu": "audiocpu.bin",
    "ym2610b:adpcma": "adpcma.bin",
    "cat702_1": "cat702_key0.bin",
    "cat702_2": "cat702_key1.bin",
}


class TaitoFX1PreparationError(Exception):
    """A MAME profile, source declaration, or local archive is unsupported."""


def _source_commit(source: Path) -> str:
    try:
        return subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise TaitoFX1PreparationError(f"cannot identify MAME checkout at {source}") from exc


def _digest(data: bytes) -> tuple[str, str]:
    import zlib
    return f"{zlib.crc32(data) & 0xffffffff:08x}", hashlib.sha1(data).hexdigest()


def load_registry(path: Path = profiles.DEFAULT_OUTPUT) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise TaitoFX1PreparationError(f"cannot read Taito FX-1 profiles {path}: {exc}") from exc
    if (document.get("schema_version") != 1 or
            document.get("mame_source_commit") != mame.PINNED_MAME_COMMIT or
            document.get("mame_driver") != profiles.DRIVER or
            len(document.get("profiles", [])) != 22):
        raise TaitoFX1PreparationError("Taito FX-1 registry is incomplete or not pinned to supported MAME")
    return document


def _filter_reviewed_upd78081(romset: mame.RomSet,
                               profile: dict[str, Any]) -> mame.RomSet:
    region = romset.regions.get(profiles.OPTIONAL_UNDUMPED)
    if region is None:
        return romset
    review = profile.get("optional_undumped_media_review")
    if (profile.get("state") != "taito_fx1s_state" or
            profile.get("machine") != "coh1000ta" or not review):
        raise TaitoFX1PreparationError("upd78081 NO_DUMP region is outside the reviewed FX1S exception")
    specs = [romset.files[op.file_index] for op in region.operations
             if isinstance(op, mame.LoadOp)]
    if (region.size != 0x2000 or len(specs) != 1 or
            specs[0].filename != "78081g503.ic655" or
            specs[0].expected_size != 0x2000 or specs[0].status != "no_dump" or
            review.get("handling") != "omitted_from_media_package_no_substitute"):
        raise TaitoFX1PreparationError("upd78081 NO_DUMP source declaration changed or is unreviewed")
    indices = {spec.index for spec in specs}
    regions = dict(romset.regions)
    del regions[profiles.OPTIONAL_UNDUMPED]
    return mame.RomSet(romset.name, regions,
                       [spec for spec in romset.files if spec.index not in indices],
                       [op for op in romset.loads if op.file_index not in indices],
                       romset.disks, romset.bios, romset.default_bios,
                       romset.source_driver)


def assemble_verified(romset: mame.RomSet, rompath: Path,
                      parents: dict[str, str] | None = None,
                      selected_bios: str | None = None,
                      archive: Path | None = None) -> tuple[dict[str, bytes], dict[int, str]]:
    """Use the generic digest-checking loader/assembler for one parsed set."""
    try:
        data, origins = mame._load_rom_files(
            romset, rompath,
            mame._lineage(romset.name, parents or {}), selected_bios, archive)
        return mame.assemble_regions(romset, data, selected_bios), origins
    except mame.PreparationError as exc:
        raise TaitoFX1PreparationError(str(exc)) from exc


def _load_set(set_name: str, source: Path, rompath: Path,
              profile: dict[str, Any], archive: Path | None) -> tuple[dict[str, bytes], dict[str, Any]]:
    try:
        romset, parents, _ = mame._find_romset(source, set_name)
    except mame.PreparationError as exc:
        raise TaitoFX1PreparationError(str(exc)) from exc
    if romset.source_driver != profiles.DRIVER:
        raise TaitoFX1PreparationError(f"{set_name} is not defined by {profiles.DRIVER}")
    if not profile.get("runtime_candidate"):
        reason = "; ".join(profile.get("unsupported_requirements", [])) or "no adapter profile"
        raise TaitoFX1PreparationError(f"{set_name} is outside the supported FX1S scope: {reason}")
    expected_tags = set(profiles.FX1S_REQUIRED_REGIONS + [profiles.OPTIONAL_UNDUMPED])
    if set(romset.regions) != expected_tags:
        raise TaitoFX1PreparationError("FX1S ROM region tags differ from the reviewed source profile")
    if romset.disks:
        raise TaitoFX1PreparationError("FX1S profile unexpectedly requires CHD/disk media")
    if romset.bios:
        raise TaitoFX1PreparationError("unexpected selectable BIOS on FX1S set")
    romset = _filter_reviewed_upd78081(romset, profile)
    regions, origins = assemble_verified(romset, rompath, parents, None, archive)
    if set(regions) != set(profiles.FX1S_REQUIRED_REGIONS):
        raise TaitoFX1PreparationError("assembled FX1S regions are incomplete")
    # The reviewed NO_DUMP removal leaves original source indices intact.
    # They identify ROM declarations, not positions in the filtered list.
    files_by_index = {spec.index: spec for spec in romset.files}
    media_sources = {files_by_index[index].filename: origin for index, origin in origins.items()}
    return regions, {"romset": set_name, "parent_chain": mame._lineage(set_name, parents),
                     "media_origins": media_sources}


def package_regions(set_name: str, profile: dict[str, Any],
                    regions: dict[str, bytes], output_dir: Path,
                    source_metadata: dict[str, Any]) -> dict[str, Any]:
    if (not profile.get("runtime_candidate") or
            profile.get("state") != "taito_fx1s_state" or
            profile.get("machine") != "coh1000ta" or
            profile.get("input") != "znt2p"):
        raise TaitoFX1PreparationError(f"{set_name} does not select the supported FX1S source configuration")
    if set(regions) != set(profiles.FX1S_REQUIRED_REGIONS):
        raise TaitoFX1PreparationError("FX1S region set is incomplete or contains unsupported data")
    expected_sizes = {row["tag"]: row["size"] for row in profile["rom_regions"]
                      if row["tag"] in profiles.FX1S_REQUIRED_REGIONS}
    actual_sizes = {tag: len(data) for tag, data in regions.items()}
    if actual_sizes != expected_sizes:
        raise TaitoFX1PreparationError(
            "FX1S ROM region sizes differ from source: " +
            ", ".join(f"{tag}={actual_sizes[tag]:#x} expected {expected_sizes[tag]:#x}"
                      for tag in sorted(expected_sizes) if actual_sizes[tag] != expected_sizes[tag]))
    payloads = {filename: regions[tag] for tag, filename in OUTPUTS.items()}
    board_ini = "\n".join([
        "format=pcsx-aow-taito-fx1-v1",
        f"mame_set={set_name}",
        f"mame_parent={profile.get('parent') or 'none'}",
        f"bios_parent={profile.get('bios_parent') or 'none'}",
        f"board_type={profile['board_type']}",
        f"board_model={profile['board_model']}",
        f"mame_machine={profile['machine']}",
        f"mame_input={profile['input']}",
        f"cpu_device={profile['cpu_device']}",
        f"cpu_clock_hz={profile['cpu_clock_hz']}",
        f"gpu_device={profile['gpu_device']}",
        f"gpu_clock_hz={profile['gpu_clock_hz']}",
        f"main_ram_size={profile['main_ram_size']}",
        f"vram_size={profile['vram_size']}",
        f"sound_cpu_device={profile['sound_cpu_device']}",
        f"sound_cpu_clock_hz={profile['sound_cpu_clock_hz']}",
        f"sound_device={profile['sound_device']}",
        f"sound_clock_hz={profile['sound_clock_hz']}",
        f"sound_latch_device={profile['sound_latch_device']}",
        f"watchdog_device={profile['watchdog_device']}",
        f"bank_select_register={profile['bank_select_register']:#x}",
        f"bank_select_mask={profile['bank_select_mask']:#04x}",
        f"watchdog_clock_bit={profile['watchdog_clock_bit']}",
        f"sample_region={profile['sample_region']}",
        "board_execution_verified=false",
        "",
    ])
    manifest: dict[str, Any] = {
        "mame_source_commit": profiles.COMMIT,
        "mame_driver": profiles.DRIVER,
        "mame_set": set_name,
        "profile": {key: profile[key] for key in (
            "machine", "input", "state", "flags", "board_model", "board_type",
            "cpu_device", "cpu_clock_hz", "gpu_device", "gpu_clock_hz",
            "main_ram_size", "vram_size", "sound_cpu_device", "sound_cpu_clock_hz",
            "sound_device", "sound_clock_hz", "sound_latch_device", "sample_region",
            "watchdog_device", "bank_select_register", "bank_select_mask",
            "watchdog_clock_bit", "bios_parent")},
        "media_source": source_metadata,
        "omitted_media": [profile["optional_undumped_media_review"]],
        "regions": {},
        "board_execution_verified": False,
    }
    for filename, data in payloads.items():
        crc, sha = _digest(data)
        manifest["regions"][filename] = {"size": len(data), "crc32": crc, "sha1": sha}
    output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="prepare-taito-fx1-", dir=output_dir.parent) as temp:
        stage = Path(temp)
        for name, data in payloads.items():
            (stage / name).write_bytes(data)
        (stage / "board.ini").write_text(board_ini, encoding="utf-8")
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        for name in [*payloads, "board.ini", "manifest.json"]:
            os.replace(stage / name, output_dir / name)
    return manifest


def prepare(set_name: str, source: Path, rompath: Path, output_dir: Path,
            archive: Path | None = None,
            profiles_path: Path = profiles.DEFAULT_OUTPUT) -> dict[str, Any]:
    registry = load_registry(profiles_path)
    if _source_commit(source) != registry["mame_source_commit"]:
        raise TaitoFX1PreparationError("MAME source checkout does not match the pinned profile registry")
    by_set = {row["set"]: row for row in registry["profiles"]}
    if set_name not in by_set:
        raise TaitoFX1PreparationError(f"no Taito FX-1 profile for MAME set {set_name}")
    profile = by_set[set_name]
    fresh = profiles.build_registry(source)
    if fresh != registry:
        raise TaitoFX1PreparationError("registry differs from current pinned MAME source")
    regions, metadata = _load_set(set_name, source, rompath, profile, archive)
    return package_regions(set_name, profile, regions, output_dir, metadata)


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--set", required=True, dest="set_name")
    parser.add_argument("--mame-source", required=True, type=Path)
    parser.add_argument("--rompath", required=True, type=Path)
    parser.add_argument("--archive", type=Path, help="optional local archive searched before MAME parent ZIPs")
    parser.add_argument("--profiles", type=Path, default=profiles.DEFAULT_OUTPUT)
    parser.add_argument("--output-dir", type=Path, default=Path("artifacts/taito-fx1"))
    args = parser.parse_args(argv)
    try:
        manifest = prepare(args.set_name, args.mame_source, args.rompath,
                           args.output_dir, args.archive, args.profiles)
    except (TaitoFX1PreparationError, profiles.TaitoFX1ProfileError,
            mame.PreparationError, OSError) as exc:
        print(f"prepare_taito_fx1_board: {exc}", file=sys.stderr)
        return 2
    print(f"prepared {args.set_name}: {len(manifest['regions'])} verified regions; board execution remains unverified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
