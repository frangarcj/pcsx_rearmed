#!/usr/bin/env python3
"""Generate Taito FX-1 set and board profiles from pinned MAME source."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

import prepare_mame_psx as mame

COMMIT = mame.PINNED_MAME_COMMIT
DRIVER = "sony/zn.cpp"
DEFAULT_SOURCE = Path(__file__).resolve().parents[2] / "mamedev"
DEFAULT_OUTPUT = Path(__file__).with_name("taito_fx1_boards.json")
OPTIONAL_UNDUMPED = "upd78081"
FX1S_REQUIRED_REGIONS = ["maincpu:rom", "cat702_1", "cat702_2", "bankedroms",
                         "audiocpu", "ym2610b:adpcma"]
FX1Z_REQUIRED_REGIONS = ["maincpu:rom", "cat702_1", "cat702_2", "bankedroms",
                         "taito_zoom:mn10200", "taito_zoom:zsg2"]
TAITO_CLASSES = ("taito_fx1s_state", "taito_fx1z_state", "gdarius_state")


class TaitoFX1ProfileError(Exception):
    """The pinned source does not support the expected FX-1 inventory."""


def _source_commit(source: Path) -> str:
    try:
        return subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise TaitoFX1ProfileError(f"cannot identify MAME checkout at {source}") from exc


def _game_rows(text: str) -> dict[str, dict[str, Any]]:
    clean = mame._without_comments(text)
    rows: dict[str, dict[str, Any]] = {}
    for match in re.finditer(r"\bGAME\s*\(", clean):
        args_text, _ = mame._call_arguments(clean, match.end() - 1)
        args = mame._split_arguments(args_text)
        if len(args) < 11:
            raise TaitoFX1ProfileError("unrecognized GAME declaration in sony/zn.cpp")
        state = args[5].strip()
        machine = args[3].strip()
        if state not in TAITO_CLASSES and machine not in ("coh1000ta", "coh1000tb", "coh1002tb"):
            continue
        name, parent = args[1].strip(), args[2].strip()
        if name in rows:
            raise TaitoFX1ProfileError(f"duplicate Taito FX-1 GAME declaration: {name}")
        rows[name] = {
            "year": int(args[0].strip()), "set": name,
            "parent": None if parent in ("0", "nullptr") else parent,
            "machine": machine, "input": args[4].strip(), "state": state,
            "init": args[6].strip(), "rotation": args[7].strip(),
            "manufacturer": args[8].strip().strip('"'),
            "description": args[9].strip().strip('"'), "flags": args[10].strip(),
        }
    return rows


def _function_body(source: str, pattern: str, name: str) -> str:
    match = re.search(pattern, source)
    if not match:
        raise TaitoFX1ProfileError(f"cannot find MAME source function {name}")
    opening = source.find("{", match.end())
    depth, pos = 1, opening + 1
    while pos < len(source) and depth:
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
        pos += 1
    if depth:
        raise TaitoFX1ProfileError(f"unterminated MAME source function {name}")
    return source[opening + 1:pos - 1]


def _machine_facts(source: str, machine: str) -> dict[str, Any]:
    body = _function_body(source,
                          r"void\s+" + re.escape(machine) +
                          r"\s*\(\s*machine_config\s*&config\s*\)", machine)
    model = next((token for token in ("zn1_1mb_vram", "zn1_2mb_vram")
                  if re.search(r"\b" + token + r"\s*\(\s*config\s*\)", body)), None)
    if model == "zn1_1mb_vram":
        model_name, vram = ("COH1000T" if machine == "coh1000ta" else "COH1000TB"), 0x100000
    elif model == "zn1_2mb_vram":
        model_name, vram = "COH1002TB", 0x200000
    else:
        raise TaitoFX1ProfileError(f"unknown Taito FX-1 geometry in config {machine}")
    cpu_setup = _function_body(source,
        r"void\s+zn_state::zn1_2mb_vram\s*\(\s*machine_config\s*&config\s*\)", "zn1_2mb_vram")
    gpu_setup = _function_body(source,
        r"void\s+zn_state::zn1_1mb_vram\s*\(\s*machine_config\s*&config\s*\)", "zn1_1mb_vram")
    base_setup = _function_body(source,
        r"void\s+zn_state::zn_base\s*\(\s*machine_config\s*&config\s*\)", "zn_base")
    if ("CXD8530CQ(config, m_maincpu, XTAL(67'737'600))" not in cpu_setup or
            "CXD8561Q(config, m_gpu, 53.693175_MHz_XTAL, 0x200000" not in cpu_setup or
            "CXD8561Q(config.replace(), m_gpu, 53.693175_MHz_XTAL, 0x100000" not in gpu_setup or
            'set_default_size("4M")' not in base_setup):
        raise TaitoFX1ProfileError("ZN-1 CPU/GPU/RAM setup differs from reviewed source")
    if "taito_fx1s_state" in body or "coh1000ta" == machine:
        board = "FX1S"
    elif "taito_fx1b(config)" in body:
        board = "FX1Z"
    else:
        raise TaitoFX1ProfileError(f"unknown Taito FX-1 daughterboard config {machine}")
    return {"board_model": model_name, "board_type": board,
            "board_family": "Taito FX-1", "main_ram_size": 0x400000,
            "vram_size": vram, "cpu_device": "CXD8530CQ",
            "cpu_clock_hz": 67737600, "gpu_device": "CXD8561Q",
            "gpu_clock_hz": 53693175}


def _fx1s_device_facts(source: str) -> dict[str, Any]:
    # Verify the selected machine setup's actual audio/control devices and
    # clocks against declarations in zn.cpp before advertising it as usable.
    s_body = _function_body(source,
        r"void\s+coh1000ta\s*\(\s*machine_config\s*&config\s*\)", "coh1000ta")
    class_match = re.search(r"class\s+taito_fx1s_state\s*:[^{]*\{", source)
    if not class_match:
        raise TaitoFX1ProfileError("cannot locate taito_fx1s_state class")
    class_body = _function_body(source, r"class\s+taito_fx1s_state\b", "taito_fx1s_state")
    sound_config = class_body
    if not all(x in sound_config for x in (
            "Z80(config, m_audiocpu, XTAL(16'000'000) / 4)",
            "YM2610B(config, m_ym2610b, 16_MHz_XTAL / 2)",
            "TC0140SYT(config, m_tc0140syt)",
            "map(0x1fb80000", "map(0x1fb80002")):
        raise TaitoFX1ProfileError("FX1S CPU/audio/sound-latch config differs from reviewed devices")
    for required in ("configure_entries(0, 8, memregion(\"audiocpu\")->base(), 0x4000)",
                     "set_entry(1)", "set_entry(data & 0x07)",
                     "map(0x0000, 0x3fff).rom()", "map(0x4000, 0x7fff).bankr(m_soundbank)",
                     "map(0xc000, 0xdfff).ram()", "map(0xe000, 0xe003).rw(m_ym2610b"):
        if required not in class_body:
            raise TaitoFX1ProfileError("FX1S sound CPU map/bank differs from reviewed source")
    if "zn1_1mb_vram(config)" not in s_body:
        raise TaitoFX1ProfileError("FX1S machine config no longer selects 1 MiB ZN-1 geometry")
    base_class = _function_body(source, r"class\s+taito_fx_state\b", "taito_fx_state")
    if "MB3773(config, m_mb3773)" not in class_body:
        raise TaitoFX1ProfileError("FX1S no longer configures the MB3773 watchdog")
    for required in ("map(0x1fb40000, 0x1fb40000)",
                     "m_rombank->set_entry(BIT(data, 0, 2))", "BIT(data, 5)"):
        if required not in base_class:
            raise TaitoFX1ProfileError("base Taito FX bank/watchdog behavior differs from reviewed source")
    return {"sound_cpu_device": "Z80", "sound_cpu_clock_hz": 4000000,
            "sound_device": "YM2610B", "sound_clock_hz": 8000000,
            "sound_latch_device": "TC0140SYT", "sample_region": "ym2610b:adpcma",
            "sample_devices": ["ADPCM-A", "ADPCM-B"],
            "watchdog_device": "MB3773", "bank_select_register": 0x1fb40000,
            "bank_select_mask": 0x03, "watchdog_clock_bit": 5}


def _fx1z_device_facts(source: str, source_dir: Path) -> dict[str, Any]:
    body = _function_body(source, r"class\s+taito_fx1z_state\b", "taito_fx1z_state")
    required = ('TAITO_ZOOM(config, m_zoom)', 'MB3773(config, m_mb3773)',
                'nvram_device::DEFAULT_ALL_1', 'set_base(m_fram.get(), 0x200)',
                'map(0x1fb00000, 0x1fb003ff)', 'map(0x1fbe0000, 0x1fbe01ff)',
                'umask32(0x00ff00ff)', 'm_spu->add_route(0, m_speaker, 0.3, 0)')
    if not all(r in body for r in required):
        raise TaitoFX1ProfileError('FX1Z composition/map differs from reviewed source')
    zoom = (source_dir / 'src/mame/sony/taito_zm.cpp').read_text()
    if not all(r in zoom for r in ("MN1020012A(config, m_soundcpu, XTAL(25'000'000)/2)",
            "TMS57002(config, m_tms57002, XTAL(25'000'000)/2)",
            "ZSG2(config, m_zsg2, XTAL(25'000'000))", 'map(0x080000, 0x0fffff).rom()')):
        raise TaitoFX1ProfileError('FX1Z Zoom clocks/ROM map differ from source')
    return {'sound_cpu_device':'MN10200', 'sound_cpu_clock_hz':12500000,
            'sound_device':'ZSG-2/TMS57002', 'sound_clock_hz':25000000,
            'sound_latch_device':'M66220FP', 'sample_region':'taito_zoom:zsg2',
            'watchdog_device':'MB3773', 'bank_select_register':0x1fb40000,
            'bank_select_mask':3, 'watchdog_clock_bit':5}


def _region_info(romset: mame.RomSet) -> list[dict[str, Any]]:
    result = []
    for region in romset.regions.values():
        files = []
        seen: set[int] = set()
        for op in region.operations:
            if not isinstance(op, mame.LoadOp) or op.file_index in seen:
                continue
            seen.add(op.file_index)
            spec = romset.files[op.file_index]
            files.append({"filename": spec.filename, "size": spec.expected_size,
                          "crc32": spec.crc32, "sha1": spec.sha1,
                          "status": spec.status, "bios_id": spec.bios_id})
        result.append({"tag": region.tag, "size": region.size, "width": region.width,
                       "endian": region.endian, "flags": region.flags,
                       "erase": region.erase, "files": files})
    return result


def _bios_parent(name: str, rows: dict[str, dict[str, Any]]) -> str | None:
    seen = {name}
    parent = rows[name].get("parent")
    while parent:
        if parent in seen:
            raise TaitoFX1ProfileError(f"cycle in Taito FX-1 parent chain at {parent}")
        seen.add(parent)
        row = rows.get(parent)
        if row is None:
            return None
        if "MACHINE_IS_BIOS_ROOT" in row["flags"]:
            return parent
        parent = row.get("parent")
    return None


def _verify_unused_upd78081(source: str, romset: mame.RomSet,
                            game: dict[str, Any]) -> dict[str, Any] | None:
    region = romset.regions.get(OPTIONAL_UNDUMPED)
    if region is None:
        return None
    if game["state"] not in TAITO_CLASSES or game["machine"] not in ("coh1000ta", "coh1000tb", "coh1002tb"):
        return None
    specs = [romset.files[op.file_index] for op in region.operations
             if isinstance(op, mame.LoadOp)]
    if region.size != 0x2000 or len(specs) != 1:
        raise TaitoFX1ProfileError("unreviewed upd78081 region geometry")
    spec = specs[0]
    if (spec.filename != "78081g503.ic655" or spec.expected_size != 0x2000
            or spec.status != "no_dump" or spec.crc32 is not None or spec.sha1 is not None):
        raise TaitoFX1ProfileError("unreviewed upd78081 NO_DUMP media declaration")
    class_body = _function_body(source, r"class\s+" + re.escape(game["state"]) + r"\b", game["state"])
    config_body = _function_body(source, r"void\s+" + re.escape(game["machine"]) + r"\s*\(\s*machine_config\s*&config\s*\)", game["machine"])
    base_body = _function_body(source, r"void\s+zn_state::zn_base\s*\(\s*machine_config\s*&config\s*\)", "zn_base")
    if "upd78081" in class_body or "UPD78081" in config_body:
        raise TaitoFX1ProfileError("FX1S now instantiates or references upd78081")
    if "ZNMCU(config, m_znmcu)" not in base_body:
        raise TaitoFX1ProfileError("common ZN setup no longer creates the separate ZNMCU")
    return {"tag": OPTIONAL_UNDUMPED, "size": 0x2000,
            "filename": spec.filename, "status": "NO_DUMP",
            "handling": "omitted_from_media_package_no_substitute",
            "source_proof": f"{game['state']}/{game['machine']} class/config do not instantiate upd78081, while zn_state::zn_base instantiates the distinct ZNMCU",
            "known_limitation": "upd78081 image remains undumped; no emulation or firmware-completeness claim is made"}


def build_registry(source_dir: Path = DEFAULT_SOURCE) -> dict[str, Any]:
    path = source_dir / "src/mame" / DRIVER
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise TaitoFX1ProfileError(f"cannot read source {path}: {exc}") from exc
    if _source_commit(source_dir) != COMMIT:
        raise TaitoFX1ProfileError("MAME checkout is not at the pinned source commit")
    source = mame._without_comments(raw)
    game_rows = _game_rows(source)
    if len(game_rows) != 22:
        raise TaitoFX1ProfileError(f"expected 22 Taito FX-1 GAME rows, found {len(game_rows)}")
    romsets, parents, coverage = mame.parse_driver(source, DRIVER)
    if len(coverage) != 116 or any(row["status"] != "parsed" for row in coverage):
        raise TaitoFX1ProfileError("generic parser did not classify every ROM_START in sony/zn.cpp")
    fx1s_devices = _fx1s_device_facts(source)
    fx1z_devices = _fx1z_device_facts(source, source_dir)
    bios_roots = [row["set"] for row in game_rows.values()
                  if "MACHINE_IS_BIOS_ROOT" in row["flags"]]
    if bios_roots != ["coh1000t"]:
        raise TaitoFX1ProfileError(f"unexpected FX-1 BIOS roots: {bios_roots}")

    profiles = []
    for game in game_rows.values():
        name = game["set"]
        romset = romsets[name]
        regions = _region_info(romset)
        region_map = {region["tag"]: region for region in regions}
        board = _machine_facts(source, game["machine"])
        is_root = "MACHINE_IS_BIOS_ROOT" in game["flags"]
        unsupported: list[str] = []
        supported = (game["state"] == "taito_fx1s_state" and
                     game["machine"] == "coh1000ta" and game["input"] == "znt2p") or (
                     game["state"] in ("taito_fx1z_state", "gdarius_state") and
                     game["machine"] in ("coh1000tb", "coh1002tb") and game["input"] in ("raystorm", "znt2p"))
        if not supported:
            unsupported.append(f"unsupported board class/config/input: {game['state']} / {game['machine']} / {game['input']}")
        required_regions = FX1S_REQUIRED_REGIONS if board["board_type"] == "FX1S" else FX1Z_REQUIRED_REGIONS
        missing = [tag for tag in required_regions if tag not in region_map]
        if missing:
            unsupported.append("missing FX1S media regions: " + ", ".join(missing))
        extras = [tag for tag in region_map if tag not in required_regions and tag != OPTIONAL_UNDUMPED]
        if extras:
            unsupported.append("unsupported FX1S regions: " + ", ".join(extras))
        if romset.disks:
            unsupported.append("requires disk/CHD media")
        required_nodump = [file["filename"] for tag in required_regions
                           for file in region_map.get(tag, {}).get("files", [])
                           if file["status"] == "no_dump"]
        if required_nodump:
            unsupported.append("required NO_DUMP media: " + ", ".join(required_nodump))
        if is_root:
            unsupported.append("BIOS root has no game program")
        if game["machine"] == "coh1000ta":
            board.update(fx1s_devices)
        optional = _verify_unused_upd78081(source, romset, game)
        if game["machine"] in ("coh1000tb", "coh1002tb"):
            board.update(fx1z_devices)
        profiles.append({**game, **board, "is_bios_root": is_root,
                         "bios_parent": _bios_parent(name, game_rows),
                         "required_regions": required_regions if supported else [],
                         "rom_regions": regions,
                         "disk_media": [disk.name for disk in romset.disks],
                         "optional_undumped_regions": [OPTIONAL_UNDUMPED] if OPTIONAL_UNDUMPED in region_map else [],
                         "optional_undumped_media_review": optional,
                         "runtime_candidate": supported and not unsupported,
                         "unsupported_requirements": unsupported})
    profiles.sort(key=lambda row: row["set"])
    return {"schema_version": 1, "mame_source_commit": COMMIT,
            "mame_driver": DRIVER, "source_sha256": hashlib.sha256(raw.encode("utf-8")).hexdigest(),
            "source_complete_rom_start_count": len(coverage),
            "family_game_count": len(profiles),
            "class_counts": {name: sum(p["state"] == name for p in profiles) for name in TAITO_CLASSES},
            "board_type_counts": {name: sum(p["board_type"] == name for p in profiles)
                                  for name in ("FX1S", "FX1Z")},
            "bios_roots": bios_roots, "fx1s_required_regions": FX1S_REQUIRED_REGIONS,
            "profiles": profiles}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame-source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args(argv)
    try:
        document = build_registry(args.mame_source)
        args.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    except (TaitoFX1ProfileError, mame.PreparationError, OSError) as exc:
        print(f"taito_fx1_boards: {exc}", file=sys.stderr)
        return 2
    print(f"wrote {document['family_game_count']} Taito FX-1 profiles to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
