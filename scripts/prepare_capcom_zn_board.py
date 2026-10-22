#!/usr/bin/env python3
"""Verify local MAME media and assemble raw Capcom ZN board regions.

The output is a media package for the adapter; this tool does not claim that
the complete Capcom board or any selected game boots.
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

import capcom_zn_boards as profiles
import prepare_mame_psx as mame

OUTPUTS = {
    "maincpu:rom": "main.bin",
    "bankedroms": "banked.bin",
    "countryrom": "country.bin",
    "audiocpu": "audiocpu.bin",
    "qsound": "qsound_samples.bin",
    "cat702_1": "cat702_key0.bin",
    "cat702_2": "cat702_key1.bin",
}


class CapcomPreparationError(Exception):
    """The source profile or local media is incomplete or unsupported."""


def _digest(data: bytes) -> tuple[str, str]:
    import zlib
    return f"{zlib.crc32(data) & 0xffffffff:08x}", hashlib.sha1(data).hexdigest()


def _source_commit(source: Path) -> str:
    try:
        return subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise CapcomPreparationError(f"cannot identify MAME checkout at {source}") from exc


def load_registry(path: Path = profiles.DEFAULT_OUTPUT) -> dict[str, Any]:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise CapcomPreparationError(f"cannot read Capcom ZN profiles {path}: {exc}") from exc
    if doc.get("schema_version") != 1 or doc.get("mame_source_commit") != mame.PINNED_MAME_COMMIT:
        raise CapcomPreparationError("Capcom ZN profile registry is not pinned to the supported MAME source")
    if doc.get("driver") != profiles.DRIVER or len(doc.get("profiles", [])) != 47:
        raise CapcomPreparationError("Capcom ZN profile registry is incomplete or has an unknown driver")
    return doc


def _filter_reviewed_unused_mcu(romset: mame.RomSet, profile: dict[str, Any]) -> mame.RomSet:
    region = romset.regions.get(profiles.OPTIONAL_UNDUMPED_REGION)
    if region is None:
        return romset
    review = profile.get("optional_undumped_media_review")
    if profile.get("state") != "capcom_zn_state" or not review:
        raise CapcomPreparationError("NO_DUMP upd78081 region is not covered by the reviewed Capcom configuration exception")
    specs = [romset.files[op.file_index] for op in region.operations
             if isinstance(op, mame.LoadOp)]
    if (region.size != 0x2000 or len(specs) != 1
            or specs[0].filename != "78081g503.ic655"
            or specs[0].expected_size != 0x2000 or specs[0].status != "no_dump"
            or review.get("handling") != "omitted_from_media_package_no_substitute"):
        raise CapcomPreparationError("unreviewed or changed upd78081 NO_DUMP declaration")
    regions = dict(romset.regions)
    del regions[profiles.OPTIONAL_UNDUMPED_REGION]
    return mame.RomSet(romset.name, regions,
                       [spec for spec in romset.files if spec.index not in {x.index for x in specs}],
                       [op for op in romset.loads if op.file_index not in {x.index for x in specs}],
                       romset.disks, romset.bios, romset.default_bios,
                       romset.source_driver)


def _load_qsound_firmware(source: Path, rompath: Path,
                          archive: Path | None) -> tuple[bytes, str]:
    source_path = source / "src" / profiles.QSOUND_DRIVER
    try:
        source_text = source_path.read_text(encoding="utf-8")
        # Registry extraction validates the sole exact ROM_IGNORE source line.
        qprofile = profiles._qsound_profile(source)
        clean_text = mame._without_comments(source_text)
        if clean_text.count("ROM_IGNORE( 0x4000 )") != 1:
            raise CapcomPreparationError("QSound ROM_IGNORE declaration differs from reviewed source")
        parse_text = clean_text.replace("ROM_IGNORE( 0x4000 )", "")
        sets, _, report = mame.parse_driver(parse_text, profiles.QSOUND_DRIVER)
    except (OSError, profiles.CapcomProfileError, mame.PreparationError) as exc:
        raise CapcomPreparationError(f"cannot verify QSound DSP source: {exc}") from exc
    romset = sets.get(qprofile["set"])
    if romset is None:
        reason = next((row["reason"] for row in report if row["set"] == qprofile["set"]), "missing")
        raise CapcomPreparationError(f"cannot parse QSound DSP: {reason}")
    if len(romset.files) != 1 or len(romset.regions) != 1:
        raise CapcomPreparationError("QSound DSP must contain only its reviewed firmware region")
    spec = romset.files[0]
    if (spec.filename != qprofile["filename"] or spec.expected_size != qprofile["size"]
            or spec.crc32 != qprofile["crc32"] or spec.sha1 != qprofile["sha1"]):
        raise CapcomPreparationError("QSound DSP ROM_LOAD differs from pinned MAME declaration")
    # MAME rom_file_size() adds ROM_IGNORE's 0x4000 to the source file length.
    # The generic assembler consumes only the first 0x2000 loaded bytes.
    from dataclasses import replace
    romset.files[0] = replace(spec, expected_size=qprofile["file_size"])
    try:
        media, origins = mame._load_rom_files(romset, rompath, [], None, archive)
        regions = mame.assemble_regions(romset, media)
    except mame.PreparationError as exc:
        raise CapcomPreparationError(f"required QSound DSP firmware {spec.filename}: {exc}") from exc
    dsp = regions[qprofile["region"]]
    if len(dsp) != qprofile["size"]:
        raise CapcomPreparationError("assembled QSound DSP region has the wrong size")
    return dsp, origins[spec.index]


def _regions_for_set(set_name: str, source: Path, rompath: Path,
                     profile: dict[str, Any], bios: str | None,
                     archive: Path | None) -> tuple[dict[str, bytes], dict[str, Any], str | None]:
    try:
        romset, parents, _ = mame._find_romset(source, set_name)
    except mame.PreparationError as exc:
        raise CapcomPreparationError(str(exc)) from exc
    if romset.source_driver != profiles.DRIVER:
        raise CapcomPreparationError(f"{set_name} is not declared by {profiles.DRIVER}")
    if not profile.get("runtime_candidate"):
        raise CapcomPreparationError(f"{set_name} is not currently packageable: {'; '.join(profile['unsupported_requirements'])}")
    if romset.disks:
        raise CapcomPreparationError("Capcom ZN media requiring CHD is unsupported")
    if set(romset.regions) != set(profile["required_regions"] + profile["optional_undumped_regions"]):
        raise CapcomPreparationError("ROM regions differ from the reviewed Capcom profile")
    selected_bios = mame._selected_bios(romset, bios)
    romset = _filter_reviewed_unused_mcu(romset, profile)
    try:
        media, origins = mame._load_rom_files(
            romset, rompath, mame._lineage(set_name, parents), selected_bios, archive)
        regions = mame.assemble_regions(romset, media, selected_bios)
    except mame.PreparationError as exc:
        raise CapcomPreparationError(str(exc)) from exc
    if set(regions) != set(profile["required_regions"]):
        raise CapcomPreparationError("assembled Capcom regions are incomplete")
    source_map = {str(index): origin for index, origin in origins.items()}
    return regions, {"romset": set_name, "selected_bios": selected_bios,
                     "media_origins": source_map}, selected_bios


def board_ini(set_name: str, profile: dict[str, Any], selected_bios: str | None) -> str:
    return "\n".join([
        "format=pcsx-aow-capcom-zn-v1",
        f"mame_set={set_name}",
        f"mame_parent={profile.get('parent') or 'none'}",
        f"bios_parent={profile.get('bios_parent') or 'none'}",
        f"bios={selected_bios or 'fixed'}",
        f"board_model={profile['board_model']}",
        f"board_family={profile['board_family']}",
        f"cpu_device={profile['cpu_device']}",
        f"cpu_clock_hz={profile['cpu_clock_hz']}",
        f"gpu_device={profile['gpu_device']}",
        f"gpu_clock_hz={profile['gpu_clock_hz']}",
        f"main_ram_size={profile['main_ram_size']}",
        f"vram_size={profile['vram_size']}",
        "board_execution_verified=false",
        "",
    ])


def package_regions(set_name: str, profile: dict[str, Any], regions: dict[str, bytes],
                    qsound_dsp: bytes, output_dir: Path,
                    source_metadata: dict[str, Any]) -> dict[str, Any]:
    if not profile.get("runtime_candidate") or profile.get("state") != "capcom_zn_state":
        raise CapcomPreparationError(f"{set_name} does not select a supported Capcom ZN class/config")
    missing = set(OUTPUTS) - set(regions)
    if missing:
        raise CapcomPreparationError("required assembled regions missing: " + ", ".join(sorted(missing)))
    expected = {entry["tag"]: entry["size"] for entry in profile["rom_regions"]
                if entry["tag"] in OUTPUTS}
    actual = {tag: len(data) for tag, data in regions.items()}
    if actual != expected:
        raise CapcomPreparationError("assembled Capcom region geometry differs from pinned profile")
    if len(qsound_dsp) != profiles.EXPECTED_QSOUND_DSP["size"]:
        raise CapcomPreparationError("QSound DSP image must assemble to exactly 0x2000 bytes")
    payloads = {filename: regions[tag] for tag, filename in OUTPUTS.items()}
    payloads["qsound_dsp.bin"] = qsound_dsp
    manifest: dict[str, Any] = {
        "mame_source_commit": profiles.COMMIT,
        "mame_driver": profiles.DRIVER,
        "mame_set": set_name,
        "profile": {key: profile[key] for key in (
            "board_model", "board_family", "cpu_device", "cpu_clock_hz",
            "gpu_device", "gpu_clock_hz", "main_ram_size", "vram_size",
            "machine", "input", "flags", "bios_parent")},
        "selected_bios": source_metadata.get("selected_bios"),
        "regions": {}, "media_source": source_metadata,
        "omitted_media": [profile["optional_undumped_media_review"]],
        "qsound_dsp_source": {**profiles.EXPECTED_QSOUND_DSP,
                              "provided_by": source_metadata.get("qsound_dsp_origin")},
        "board_execution_verified": False,
    }
    for filename, data in payloads.items():
        crc, sha = _digest(data)
        manifest["regions"][filename] = {"size": len(data), "crc32": crc, "sha1": sha}
    output_dir.mkdir(parents=True, exist_ok=True)
    # Stage files, then replace only the known package artifacts. Existing
    # unrelated user files in the destination are preserved.
    with tempfile.TemporaryDirectory(prefix=".prepare-capcom-zn-", dir=output_dir.parent) as temp:
        stage = Path(temp)
        for name, data in payloads.items():
            (stage / name).write_bytes(data)
        (stage / "board.ini").write_text(
            board_ini(set_name, profile, source_metadata.get("selected_bios")), encoding="utf-8")
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        for name in [*payloads, "board.ini", "manifest.json"]:
            os.replace(stage / name, output_dir / name)
    return manifest


def prepare(set_name: str, source: Path, rompath: Path, output_dir: Path,
            bios: str | None = None, archive: Path | None = None,
            qsound_archive: Path | None = None,
            profiles_path: Path = profiles.DEFAULT_OUTPUT) -> dict[str, Any]:
    registry = load_registry(profiles_path)
    if _source_commit(source) != registry["mame_source_commit"]:
        raise CapcomPreparationError("MAME source checkout differs from the pinned Capcom profile revision")
    selected = {row["set"]: row for row in registry["profiles"]}
    if set_name not in selected:
        raise CapcomPreparationError(f"no Capcom ZN board profile for MAME set {set_name}")
    profile = selected[set_name]
    # Re-extract from source each run so edited/stale JSON cannot authorize a
    # layout or firmware exception that current source does not declare.
    fresh = profiles.build_registry(source, profiles.DEFAULT_INVENTORY)
    fresh_profile = next(row for row in fresh["profiles"] if row["set"] == set_name)
    if fresh_profile != profile or fresh["qsound_dsp"] != registry["qsound_dsp"]:
        raise CapcomPreparationError("profile registry differs from current pinned MAME source")
    regions, metadata, _ = _regions_for_set(set_name, source, rompath, profile, bios, archive)
    dsp, dsp_origin = _load_qsound_firmware(source, rompath,
                                            qsound_archive or archive)
    metadata["qsound_dsp_origin"] = dsp_origin
    return package_regions(set_name, profile, regions, dsp, output_dir, metadata)


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--set", required=True, dest="set_name")
    parser.add_argument("--mame-source", required=True, type=Path)
    parser.add_argument("--rompath", required=True, type=Path)
    parser.add_argument("--archive", type=Path, help="optional game/BIOS archive searched first")
    parser.add_argument("--qsound-archive", type=Path, help="optional local archive containing dl-1425.bin")
    parser.add_argument("--bios", choices=("j", "e"), help="COH1000C BIOS; defaults to MAME's Japanese BIOS")
    parser.add_argument("--profiles", type=Path, default=profiles.DEFAULT_OUTPUT)
    parser.add_argument("--output-dir", type=Path, default=Path("artifacts/capcom-zn"))
    args = parser.parse_args(argv)
    try:
        result = prepare(args.set_name, args.mame_source, args.rompath, args.output_dir,
                         args.bios, args.archive, args.qsound_archive, args.profiles)
    except (CapcomPreparationError, profiles.CapcomProfileError,
            mame.PreparationError, OSError) as exc:
        print(f"prepare_capcom_zn_board: {exc}", file=sys.stderr)
        return 2
    print(f"prepared {args.set_name}: {len(result['regions'])} verified regions; board execution remains unverified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
