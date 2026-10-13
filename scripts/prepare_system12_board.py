#!/usr/bin/env python3
"""Package assembled MAME System 12 regions into the native board layout.

The input is produced by prepare_mame_psx.py. This validates the pinned source
manifest and every region digest, then writes the four raw regions plus a
strict board.cfg, optional default EEPROM, and a supplied local CDXA CHD.
The runtime refuses profiles with declared unsupported devices.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
import zlib
from pathlib import Path

COMMIT = "deb2f54fe7300e7a192690318beb860159bfd52a"
HERE = Path(__file__).resolve().parent
REGISTRY = HERE / "system12_boards.json"
REGION_NAMES = {
    "maincpu:rom": "main.bin",
    "bankedroms": "banked.bin",
    "sub": "sub.bin",
    "c352": "wave.bin",
}


class BoardPreparationError(Exception):
    pass


def _digest(data: bytes) -> tuple[str, str]:
    return f"{zlib.crc32(data) & 0xffffffff:08x}", hashlib.sha1(data).hexdigest()


def prepare(input_dir: Path, output_dir: Path,
            registry_path: Path = REGISTRY, chd: Path | None = None) -> dict[str, object]:
    try:
        manifest = json.loads((input_dir / "manifest.json").read_text(encoding="utf-8"))
        registry = json.loads(registry_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise BoardPreparationError(f"cannot read assembled manifest or board registry: {exc}") from exc
    if (registry.get("schema_version") != 1 or
            registry.get("mame_source_commit") != COMMIT):
        raise BoardPreparationError("board registry is not from the pinned MAME source")
    if (manifest.get("mame_source_commit") != COMMIT or
            manifest.get("rom_regions_assembled") is not True):
        raise BoardPreparationError("input must be assembled from the pinned MAME source")
    matches = [item for item in registry.get("sets", [])
               if item.get("set") == manifest.get("mame_set")]
    if len(matches) != 1:
        raise BoardPreparationError(f"no unique System 12 profile for {manifest.get('mame_set')!r}")
    profile = matches[0]
    if manifest.get("mame_driver") != "namco/namcos12.cpp":
        raise BoardPreparationError("input is not a Namco System 12 ROM set")
    regions = manifest.get("regions")
    if not isinstance(regions, dict):
        raise BoardPreparationError("manifest has no region table")
    contents: dict[str, bytes] = {}
    region_names = dict(REGION_NAMES)
    if "at28c16" in regions:
        region_names["at28c16"] = "eeprom.bin"
    for tag, output_name in region_names.items():
        record = regions.get(tag)
        if not isinstance(record, dict):
            raise BoardPreparationError(f"required region {tag} is missing")
        filename = record.get("file")
        if not isinstance(filename, str) or Path(filename).name != filename:
            raise BoardPreparationError(f"unsafe region filename for {tag}")
        try:
            data = (input_dir / filename).read_bytes()
        except OSError as exc:
            raise BoardPreparationError(f"cannot read {tag}: {exc}") from exc
        crc, sha = _digest(data)
        if (len(data) != record.get("size") or crc != str(record.get("crc32", "")).lower().zfill(8)
                or sha != str(record.get("sha1", "")).lower()):
            raise BoardPreparationError(f"assembled region {tag} failed size/CRC32/SHA1 verification")
        contents[output_name] = data
    disks = manifest.get("pending_disk_media", [])
    disk_name = None
    if disks:
        if len(disks) != 1 or chd is None:
            raise BoardPreparationError("this board requires its local disc: pass --chd PATH")
        disk = disks[0]
        disk_name = disk["name"] + ".chd"
        if Path(disk_name).name != disk_name:
            raise BoardPreparationError("unsafe disc filename")
        with chd.open("rb") as stream:
            header = stream.read(124)
        if (len(header) != 124 or header[:8] != b"MComprHD" or
                int.from_bytes(header[12:16], "big") != 5 or
                header[84:104].hex() != disk["sha1"]):
            raise BoardPreparationError("CHD header/version/SHA1 does not match the pinned disc")
    values = {
        "schema": "1", "mame_set": profile["set"], "machine": profile["machine"],
        "state": profile["state"], "ram_size": str(profile["ram_size"]),
        "main_size": str(len(contents["main.bin"])),
        "bank_size": str(len(contents["banked.bin"])),
        "sub_size": str(len(contents["sub.bin"])),
        "wave_size": str(len(contents["wave.bin"])),
        "boot_hack": "1" if profile["boot_hack"] else "0",
        "alt_bank": "1" if profile["alt_bank"] else "0",
        "tektagt_dma": "1" if profile["tektagt_dma"] else "0",
        "unsupported": ",".join(profile["unsupported"]) or "none",
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, data in contents.items():
        (output_dir / name).write_bytes(data)
    if disk_name:
        shutil.copyfile(chd, output_dir / disk_name)
    (output_dir / "board.cfg").write_text("".join(f"{key}={value}\n" for key, value in values.items()), encoding="ascii")
    output_manifest = {
        "system": "Namco System 12", "mame_set": profile["set"],
        "mame_source_commit": COMMIT, "profile": profile,
        "regions": {name: {"size": len(data), "crc32": _digest(data)[0],
                           "sha1": _digest(data)[1]} for name, data in contents.items()},
        "unsupported": profile["unsupported"],
        "board_execution_verified": False,
    }
    (output_dir / "system12_manifest.json").write_text(
        json.dumps(output_manifest, indent=2) + "\n", encoding="utf-8")
    return output_manifest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assembled_dir", type=Path, help="directory from prepare_mame_psx.py")
    parser.add_argument("-o", "--output-dir", type=Path, required=True)
    parser.add_argument("--registry", type=Path, default=REGISTRY)
    parser.add_argument("--chd", type=Path, help="local CHD for a CDXA board")
    args = parser.parse_args(argv)
    try:
        manifest = prepare(args.assembled_dir, args.output_dir, args.registry, args.chd)
    except (BoardPreparationError, OSError) as exc:
        print(f"prepare_system12_board: error: {exc}", file=sys.stderr)
        return 1
    unsupported = manifest["unsupported"]
    status = "unsupported requirements: " + ", ".join(unsupported) if unsupported else "board profile packaged"
    print(f"Packaged {manifest['mame_set']} ({status}); execution verification is separate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
