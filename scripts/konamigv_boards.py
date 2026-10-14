#!/usr/bin/env python3
"""Generate source-audited Konami System GV set and media profiles.

Copyright (c) 2026 OpenAI. All rights reserved.
This file contains no MAME implementation code; hardware/media facts are
extracted from the separately licensed upstream source identified below.
"""
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
DRIVER = "konami/konamigv.cpp"
DEFAULT_SOURCE = Path(__file__).resolve().parents[2] / "mamedev"
DEFAULT_OUTPUT = Path(__file__).with_name("konamigv_boards.json")
INVENTORY = Path(__file__).with_name("mame_psx_systems.json")
GAME_CLASSES = ("konamigv_state", "tokimeki_state", "simpbowl_state")


class KonamiGVProfileError(Exception):
    """Pinned source or inventory does not match the reviewed System GV map."""


def _source_commit(source: Path) -> str:
    try:
        return subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise KonamiGVProfileError(f"cannot identify MAME checkout at {source}") from exc


def _game_rows(text: str) -> dict[str, dict[str, Any]]:
    clean = mame._without_comments(text)
    rows: dict[str, dict[str, Any]] = {}
    for match in re.finditer(r"\bGAME\s*\(", clean):
        args_text, _ = mame._call_arguments(clean, match.end() - 1)
        args = mame._split_arguments(args_text)
        if len(args) < 11:
            raise KonamiGVProfileError("unrecognized GAME declaration in konami/konamigv.cpp")
        name, parent = args[1].strip(), args[2].strip()
        if name in rows:
            raise KonamiGVProfileError(f"duplicate System GV GAME declaration: {name}")
        rows[name] = {
            "year": int(args[0].strip()), "set": name,
            "parent": None if parent in ("0", "nullptr") else parent,
            "machine": args[3].strip(), "input": args[4].strip(),
            "state": args[5].strip(), "init": args[6].strip(),
            "rotation": args[7].strip(), "manufacturer": args[8].strip().strip('"'),
            "description": args[9].strip().strip('"'), "flags": args[10].strip(),
        }
    return rows


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


def _disk_bad_dump(source: str, set_name: str, disk_name: str) -> bool:
    block_match = re.search(r"\bROM_START\s*\(\s*" + re.escape(set_name) +
                            r"\s*\)(.*?)\bROM_END", source, re.S)
    if not block_match:
        raise KonamiGVProfileError(f"cannot locate ROM_START({set_name})")
    for match in re.finditer(r"\bDISK_IMAGE(?:_READONLY)?\s*\(", block_match.group(1)):
        args, _ = mame._call_arguments(block_match.group(1), match.end() - 1)
        values = mame._split_arguments(args)
        if values and mame._string_arg(values[0]) == disk_name:
            return "BAD_DUMP" in " ".join(values[2:])
    raise KonamiGVProfileError(f"cannot verify CHD quality marker for {set_name}/{disk_name}")


def _source_hardware(source: str) -> dict[str, Any]:
    required = (
        "CXD8530BQ(config, m_maincpu, XTAL(67'737'600))",
        'set_default_size("2M")',
        'CXD8514Q(config, "gpu", XTAL(53\'693\'175), 0x100000',
        "NCR53CF96(config, m_ncr53cf96, 32_MHz_XTAL/2)",
        "MB89371(config, m_duart, 4_MHz_XTAL)",
        'EEPROM_93C46_16BIT(config, "eeprom")',
        'NSCSI_XM5401(config, "cdrom")',
    )
    if not all(token in source for token in required):
        raise KonamiGVProfileError("common System GV hardware config differs from reviewed source")
    for token in ("void konamigv_state::btchamp_map", "void konamigv_state::kdeadeye_map",
                  "void simpbowl_state::simpbowl_map", "void tokimeki_state::tmosh_map",
                  "SHARP_LH28F400(config, \"flash\")", "fujitsu_29f016a_device"):
        if token not in source:
            raise KonamiGVProfileError(f"cannot verify System GV device/map fact: {token}")
    return {
        "board_family": "Konami System GV / Baby Phoenix",
        "cpu_device": "CXD8530BQ", "cpu_clock_hz": 67737600,
        "main_ram_size": 0x200000, "gpu_device": "CXD8514Q",
        "gpu_clock_hz": 53693175, "vram_size": 0x100000,
        "scsi_device": "NCR53CF96", "scsi_clock_hz": 16000000,
        "cdrom_device": "XM-5401", "eeprom_device": "93C46 16-bit",
        "duart_device": "MB89371", "duart_clock_hz": 4000000,
        "spu_clock_hz": 33868800, "common_map": "konamigv_map",
    }


def _io_class_facts(game: dict[str, Any]) -> tuple[list[str], list[str]]:
    state, machine, inp = game["state"], game["machine"], game["input"]
    if machine not in ("konamigv", "btchamp", "kdeadeye", "tmosh", "simpbowl"):
        raise KonamiGVProfileError(f"unknown System GV machine config {machine}")
    if state == "konamigv_state" and machine in ("konamigv",):
        return (["P1/P2/P3_P4 active-low port words", "MB89371 dual UART", "93C46 EEPROM"], [])
    if state == "konamigv_state" and machine == "btchamp":
        return (["P1/P2 active-low port words", "dual UPD4701A trackballs", "Sharp LH28F400 flash", "MB89371 dual UART", "93C46 EEPROM"],
                [])
    if state == "konamigv_state" and machine == "kdeadeye":
        return (["gun X/Y ports for both players", "buttons port", "Sharp LH28F400 flash", "MB89371 dual UART", "93C46 EEPROM"],
                ["two lightguns and crosshair inputs", "flash command protocol"])
    if state == "tokimeki_state" and machine == "tmosh" and inp == "tmosh":
        return (["tokimeki serial sensor/printer port", "heartbeat sensor", "GSR sensor", "printer timing/video", "MB89371 dual UART", "93C46 EEPROM"],
                ["Tokimeki sensor/printer controller and print media path"])
    if state == "simpbowl_state" and machine == "simpbowl" and inp == "simpbowl":
        return (["Simpsons Bowling 4x Fujitsu flash map", "UPD4701 trackball", "MB89371 dual UART", "93C46 EEPROM"],
                [])
    raise KonamiGVProfileError(f"unreviewed System GV state/input pair {state}/{inp}")


def _listxml_inventory(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise KonamiGVProfileError(f"cannot read pinned listxml inventory {path}: {exc}") from exc
    if (data.get("source", {}).get("mame_commit") != COMMIT or
            not isinstance(data.get("systems"), list)):
        raise KonamiGVProfileError("listxml inventory is not pinned to the expected MAME commit")
    matches = [row for row in data["systems"] if row.get("driver") == DRIVER]
    if len(matches) != 1 or matches[0].get("set_count") != 15:
        raise KonamiGVProfileError("pinned listxml inventory must contain all 15 System GV sets")
    return {row["set"]: row for row in matches[0]["sets"]}


def build_registry(source_dir: Path = DEFAULT_SOURCE,
                   inventory_path: Path = INVENTORY) -> dict[str, Any]:
    path = source_dir / "src/mame" / DRIVER
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise KonamiGVProfileError(f"cannot read source {path}: {exc}") from exc
    if _source_commit(source_dir) != COMMIT:
        raise KonamiGVProfileError("MAME checkout is not at the pinned source commit")
    source = mame._without_comments(raw)
    rows = _game_rows(source)
    if len(rows) != 15:
        raise KonamiGVProfileError(f"expected 15 System GV GAME rows, found {len(rows)}")
    romsets, parents, coverage = mame.parse_driver(source, DRIVER)
    if len(coverage) != 15 or any(row["status"] != "parsed" for row in coverage):
        raise KonamiGVProfileError("generic parser did not classify all 15 System GV ROM_START blocks")
    listxml = _listxml_inventory(inventory_path)
    if set(listxml) != set(rows):
        raise KonamiGVProfileError("pinned listxml sets differ from System GV source GAME declarations")
    hardware = _source_hardware(source)

    profiles = []
    for game in rows.values():
        name = game["set"]
        romset = romsets[name]
        region_rows = _region_info(romset)
        region_map = {r["tag"]: r for r in region_rows}
        ls = listxml[name]
        if (ls.get("driver") != DRIVER or ls.get("description") != game["description"] or
                ls.get("romof") != game["parent"]):
            raise KonamiGVProfileError(f"pinned listxml metadata disagrees for {name}")
        io_devices, unsupported_io = _io_class_facts(game)
        rom_disk_rows = []
        for disk in romset.disks:
            if not disk.sha1 or disk.region != "cdrom" or not disk.readonly:
                raise KonamiGVProfileError(f"unreviewed CHD declaration for {name}/{disk.name}")
            rom_disk_rows.append({"name": disk.name, "region": disk.region,
                                  "index": disk.index, "sha1": disk.sha1,
                                  "readonly": disk.readonly,
                                  "status": "BAD_DUMP" if _disk_bad_dump(raw, name, disk.name) else "GOOD"})
        missing = []
        if name != "konamigv":
            if not rom_disk_rows:
                missing.append("no declared game CD image")
            if "eeprom" not in region_map or region_map["eeprom"]["size"] != 0x80:
                missing.append("missing 128-byte game EEPROM region")
        if any(file["status"] == "no_dump" for region in region_rows for file in region["files"]):
            missing.append("required NO_DUMP ROM media")
        root = "MACHINE_IS_BIOS_ROOT" in game["flags"]
        unsupported = list(unsupported_io)
        if root:
            unsupported.append("BIOS-only root has no game program or game-specific EEPROM")
        profiles.append({
            **game, **hardware, "source_class": game["state"],
            "machine_config": game["machine"], "input_config": game["input"],
            "is_bios_root": root, "bios_parent": "konamigv" if not root else None,
            "listxml": {"cloneof": ls.get("cloneof"), "romof": ls.get("romof"),
                        "isbios": ls.get("isbios"), "runnable": ls.get("runnable"),
                        "mame_driver": ls.get("mame_driver"),
                        "mame_features": ls.get("mame_features", []),
                        "media_devices": ls.get("media_devices", [])},
            "source_io_devices": io_devices,
            "unsupported_game_io": unsupported,
            "rom_regions": region_rows,
            "disk_media": rom_disk_rows,
            "media_candidate": not root and not missing,
            "known_media_issues": missing + [
                f"CHD {d['name']} is MAME-marked BAD_DUMP" for d in rom_disk_rows
                if d["status"] == "BAD_DUMP"],
            "game_execution_verified": False,
        })
    profiles.sort(key=lambda row: row["set"])
    return {"schema_version": 1, "mame_source_commit": COMMIT,
            "mame_driver": DRIVER,
            "source_sha256": hashlib.sha256(raw.encode("utf-8")).hexdigest(),
            "listxml_inventory": "scripts/mame_psx_systems.json",
            "listxml_source_version": "0.287 (unknown)",
            "source_complete_rom_start_count": len(coverage),
            "family_game_count": len(profiles),
            "source_class_counts": {name: sum(p["state"] == name for p in profiles)
                                     for name in (*GAME_CLASSES, "konamigv_state")},
            "machine_config_counts": {name: sum(p["machine"] == name for p in profiles)
                                       for name in ("konamigv", "btchamp", "kdeadeye", "tmosh", "simpbowl")},
            "bios_root": "konamigv", "profiles": profiles}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame-source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--inventory", type=Path, default=INVENTORY)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    args = parser.parse_args(argv)
    try:
        document = build_registry(args.mame_source, args.inventory)
        args.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    except (KonamiGVProfileError, mame.PreparationError, OSError) as exc:
        print(f"konamigv_boards: {exc}", file=sys.stderr)
        return 2
    print(f"wrote {document['family_game_count']} System GV profiles to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
