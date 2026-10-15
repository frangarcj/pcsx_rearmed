#!/usr/bin/env python3
"""Validate local System GV ROM/EEPROM/CHD media and package board regions.

No ROM or CHD is downloaded. A successful package is a verified media set;
it does not claim that the System GV board or its game-specific peripherals
are emulated.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Any, Iterable

import prepare_mame_psx as mame
import konamigv_boards as boards

DRIVER = boards.DRIVER
REGION_FILES = {"maincpu:rom": "bios.bin", "eeprom": "eeprom.bin", "flash": "flash.bin"}
PACKAGE_FILES = {"board.ini", "manifest.json", *REGION_FILES.values()}
KNOWN_GV_SETS = {
    "btchamp", "hyperath", "kdeadeye", "konamigv", "lacrazyc", "nagano98",
    "naganoj", "powyak96", "simpbowl", "susume", "tmosh", "tmoshs",
    "tmoshsp", "tmoshspa", "weddingr",
}


class KonamiGVPreparationError(Exception):
    """A profile, local ROM/CHD, or output layout is unsupported or invalid."""


def load_registry(path: Path = boards.DEFAULT_OUTPUT) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise KonamiGVPreparationError(f"cannot read System GV profiles {path}: {exc}") from exc
    if not isinstance(document, dict):
        raise KonamiGVPreparationError("System GV registry root must be an object")
    rows = document.get("profiles")
    if (type(document.get("schema_version")) is not int or document.get("schema_version") != 1 or
            document.get("mame_source_commit") != mame.PINNED_MAME_COMMIT or
            document.get("mame_driver") != DRIVER or
            type(document.get("family_game_count")) is not int or document.get("family_game_count") != 15 or
            not isinstance(rows, list) or len(rows) != 15):
        raise KonamiGVPreparationError("System GV registry is incomplete or not pinned to MAME")
    if any(not isinstance(row, dict) for row in rows):
        raise KonamiGVPreparationError("System GV registry contains a malformed profile row")
    names = [row.get("set") for row in rows]
    if (any(not isinstance(name, str) for name in names) or len(set(names)) != 15 or
            set(names) != KNOWN_GV_SETS):
        raise KonamiGVPreparationError("System GV registry contains duplicate or unknown set rows")
    for row in rows:
        _validate_profile_shape(row)
    return document


def _validate_profile_shape(profile: Any) -> None:
    """Reject malformed profile objects before any indexing or output changes."""
    if not isinstance(profile, dict):
        raise KonamiGVPreparationError("System GV profile must be an object")
    set_name = profile.get("set")
    if not isinstance(set_name, str) or set_name not in KNOWN_GV_SETS:
        raise KonamiGVPreparationError("System GV profile has an unknown set name")
    if type(profile.get("year")) is not int or profile["year"] < 1990:
        raise KonamiGVPreparationError(f"System GV profile {set_name} has an invalid year")
    if type(profile.get("is_bios_root")) is not bool:
        raise KonamiGVPreparationError(f"System GV profile {set_name} has an invalid BIOS-root flag")
    regions = profile.get("rom_regions")
    if not isinstance(regions, list) or not regions:
        raise KonamiGVPreparationError(f"System GV profile {set_name} has no ROM regions")
    tags: list[str] = []
    for region in regions:
        if not isinstance(region, dict):
            raise KonamiGVPreparationError(f"System GV profile {set_name} has a malformed ROM region")
        tag, size = region.get("tag"), region.get("size")
        if not isinstance(tag, str) or tag not in REGION_FILES:
            raise KonamiGVPreparationError(f"System GV profile {set_name} has an unmapped ROM region")
        if type(size) is not int or size <= 0:
            raise KonamiGVPreparationError(f"System GV profile {set_name} has an invalid region size")
        tags.append(tag)
    if len(tags) != len(set(tags)) or "maincpu:rom" not in tags:
        raise KonamiGVPreparationError(f"System GV profile {set_name} has duplicate or missing region tags")
    if "eeprom" not in tags or ("flash" in tags) != (set_name == "kdeadeye"):
        raise KonamiGVPreparationError(f"System GV profile {set_name} has an unexpected region map")
    disks = profile.get("disk_media")
    if not isinstance(disks, list) or len(disks) != (0 if profile["is_bios_root"] else 1):
        raise KonamiGVPreparationError(f"System GV profile {set_name} has invalid disk media")
    for disk in disks:
        if not isinstance(disk, dict):
            raise KonamiGVPreparationError(f"System GV profile {set_name} has malformed disk media")
        name = disk.get("name")
        if (not isinstance(name, str) or not name or Path(name).name != name or
                "/" in name or "\\" in name):
            raise KonamiGVPreparationError(f"System GV profile {set_name} has an unsafe disk name")
        if type(disk.get("index")) is not int or disk["index"] < 0:
            raise KonamiGVPreparationError(f"System GV profile {set_name} has invalid disk index")
        if type(disk.get("readonly")) is not bool:
            raise KonamiGVPreparationError(f"System GV profile {set_name} has invalid disk readonly flag")
        if not isinstance(disk.get("sha1"), str) or not re.fullmatch(r"[0-9a-fA-F]{40}", disk["sha1"]):
            raise KonamiGVPreparationError(f"System GV profile {set_name} has invalid disk SHA1")
    if profile["is_bios_root"] != (set_name == "konamigv"):
        raise KonamiGVPreparationError(f"System GV profile {set_name} has inconsistent BIOS-root identity")


def _validated_old_manifest(manifest_path: Path) -> tuple[dict[str, Any], set[str]]:
    try:
        old = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise KonamiGVPreparationError("existing output has an invalid manifest; refusing merge") from exc
    if not isinstance(old, dict):
        raise KonamiGVPreparationError("existing output manifest must be an object; refusing merge")
    if (old.get("system") != "Konami System GV" or old.get("mame_driver") != DRIVER or
            old.get("mame_source_commit") != boards.COMMIT):
        raise KonamiGVPreparationError("existing output is not a package from the pinned System GV source")
    old_set = old.get("mame_set")
    try:
        registry = load_registry()
    except KonamiGVPreparationError:
        raise
    profile = next((row for row in registry["profiles"] if row["set"] == old_set), None)
    if profile is None or profile["is_bios_root"]:
        raise KonamiGVPreparationError("existing output manifest names an unknown System GV game")
    old_regions = old.get("regions")
    if not isinstance(old_regions, dict):
        raise KonamiGVPreparationError("existing output manifest has malformed regions; refusing merge")
    allowed = {f"{disk['name']}.chd" for disk in profile["disk_media"]}
    allowed.update(REGION_FILES[tag] for tag in (region["tag"] for region in profile["rom_regions"]))
    if (any(not isinstance(name, str) for name in old_regions) or
            not set(old_regions).issubset(allowed)):
        raise KonamiGVPreparationError("existing output manifest claims files outside its GV profile")
    for metadata in old_regions.values():
        if (not isinstance(metadata, dict) or type(metadata.get("size")) is not int or
                metadata["size"] < 0 or not isinstance(metadata.get("crc32"), str) or
                not re.fullmatch(r"[0-9a-fA-F]{8}", metadata["crc32"]) or
                not isinstance(metadata.get("sha1"), str) or
                not re.fullmatch(r"[0-9a-fA-F]{40}", metadata["sha1"])):
            raise KonamiGVPreparationError("existing output manifest has invalid region metadata")
    return old, set(old_regions)


def _source_commit(source: Path) -> str:
    try:
        return subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                              check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise KonamiGVPreparationError(f"cannot identify MAME checkout at {source}") from exc


def _verify_chd(path: Path, disk: dict[str, Any], chdman: str | None = None) -> dict[str, Any]:
    tool = chdman or shutil.which("chdman")
    if not tool:
        raise KonamiGVPreparationError("chdman is required to validate CHD header, metadata, and SHA1")
    try:
        info = subprocess.run([tool, "info", "-i", str(path), "-v"],
                              check=True, capture_output=True, text=True).stdout
        subprocess.run([tool, "verify", "-i", str(path)], check=True,
                       capture_output=True, text=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        detail = getattr(exc, "stderr", "") or str(exc)
        raise KonamiGVPreparationError(f"chdman rejected {path.name}: {detail.strip()}") from exc
    def field(name: str) -> str | None:
        match = re.search(r"^\s*" + re.escape(name) + r":\s*(\S.*?)\s*$", info, re.M)
        return match.group(1).strip() if match else None
    version, logical_size, sha = field("File Version"), field("Logical size"), field("SHA1")
    size_match = re.fullmatch(r"([0-9][0-9,]*)\s+bytes", logical_size or "", re.I)
    if version not in ("3", "4", "5") or not size_match or int(size_match.group(1).replace(",", "")) <= 0:
        raise KonamiGVPreparationError(f"{path.name} is not a supported CHD image")
    if sha is None or sha.lower() != disk["sha1"].lower():
        raise KonamiGVPreparationError(f"{path.name} CHD header SHA1 does not match MAME {disk['sha1']}")
    # MAME's cdrom_file accepts both legacy CHTR and current CHT2 per-track
    # metadata. Validate that this is a recognized CD layout without imposing
    # one particular data/audio track arrangement on the source declaration.
    supported_types = {"AUDIO", "MODE1", "MODE1/2048", "MODE1_RAW", "MODE1/2352",
                       "MODE2", "MODE2/2336", "MODE2_FORM1", "MODE2/2048",
                       "MODE2_FORM2", "MODE2/2324", "MODE2_FORM_MIX", "MODE2_RAW",
                       "MODE2/2352"}
    track_rows = []
    metadata_headers = list(re.finditer(r"Metadata:\s+Tag='(CHTR|CHT2)'[^\n]*\n"
                                        r"\s*TRACK:(\d+) TYPE:([A-Z0-9_/]+)", info))
    for match in metadata_headers:
        number, kind = int(match.group(2)), match.group(3)
        if number < 1 or kind not in supported_types:
            raise KonamiGVPreparationError(f"{path.name} contains unsupported CD track metadata")
        track_rows.append({"number": number, "type": kind, "tag": match.group(1)})
    if not track_rows:
        raise KonamiGVPreparationError(f"{path.name} has no recognized CHTR/CHT2 CD track metadata")
    if len({row["number"] for row in track_rows}) != len(track_rows):
        raise KonamiGVPreparationError(f"{path.name} contains duplicate CD track numbers")
    return {"file_version": int(version), "logical_size": logical_size,
            "header_sha1": sha.lower(), "tracks": track_rows}


def _load_regions(set_name: str, source: Path, rompath: Path,
                  archive: Path | None) -> tuple[dict[str, bytes], dict[str, Any]]:
    try:
        romset, parents, _ = mame._find_romset(source, set_name)
        media, origins = mame._load_rom_files(
            romset, rompath, mame._lineage(set_name, parents), None, archive)
        regions = mame.assemble_regions(romset, media)
    except mame.PreparationError as exc:
        raise KonamiGVPreparationError(str(exc)) from exc
    return regions, {"rom_set": romset.name, "parent_chain": mame._lineage(set_name, parents),
                     "rom_files": [{"filename": spec.filename, "size": spec.expected_size,
                                    "crc32": spec.crc32, "sha1": spec.sha1,
                                    "status": spec.status, "provided_by": origins.get(spec.index)}
                                   for spec in romset.files]}


def _disk_file(disk: dict[str, Any], set_name: str, rompath: Path,
               archive: Path | None) -> tuple[Path, bool]:
    candidates = [rompath / set_name / f"{disk['name']}.chd",
                  rompath / f"{disk['name']}.chd",
                  rompath / set_name / disk["name"], rompath / disk["name"]]
    for candidate in candidates:
        if candidate.is_file():
            return candidate, False
    if archive and archive.is_file():
        try:
            with zipfile.ZipFile(archive) as zf:
                matches = [name for name in zf.namelist()
                           if Path(name).name.lower() in (f"{disk['name']}.chd".lower(),
                                                          disk["name"].lower())]
                if len(matches) > 1:
                    raise KonamiGVPreparationError(f"multiple CHD entries for {disk['name']} in archive")
                if matches:
                    fd, temp_name = tempfile.mkstemp(prefix="system-gv-", suffix=".chd")
                    os.close(fd)
                    extracted = Path(temp_name)
                    with zf.open(matches[0]) as src, extracted.open("wb") as dst:
                        shutil.copyfileobj(src, dst)
                    return extracted, True
        except (OSError, zipfile.BadZipFile) as exc:
            raise KonamiGVPreparationError(f"cannot read CHD archive {archive}: {exc}") from exc
    raise KonamiGVPreparationError(
        f"missing local CHD {disk['name']}.chd (searched set directory, ROM path, and archive)")


def _payloads(profile: dict[str, Any], regions: dict[str, bytes],
              rompath: Path, archive: Path | None,
              chdman: str | None,
              disk_links: dict[str, Path] | None = None) -> tuple[dict[str, bytes], dict[str, Any]]:
    expected = {row["tag"]: row["size"] for row in profile.get("rom_regions", [])}
    if set(regions) != set(expected):
        raise KonamiGVPreparationError(
            "assembled System GV region table differs from source; expected " +
            ", ".join(sorted(expected)))
    bad = [f"{tag} {len(regions[tag]):#x}!={size:#x}" for tag, size in expected.items()
           if len(regions[tag]) != size]
    if bad:
        raise KonamiGVPreparationError("System GV region size mismatch: " + ", ".join(bad))
    if set(regions) - set(REGION_FILES):
        raise KonamiGVPreparationError("unreviewed System GV region(s): " +
                                       ", ".join(sorted(set(regions) - set(REGION_FILES))))
    if profile.get("is_bios_root"):
        if profile["set"] != "konamigv" or "eeprom" not in regions or profile["disk_media"]:
            raise KonamiGVPreparationError("unreviewed System GV BIOS-root geometry")
    elif len(profile.get("disk_media", [])) != 1:
        raise KonamiGVPreparationError("game set must declare exactly one CD CHD image")
    payloads = {REGION_FILES[tag]: data for tag, data in regions.items()}
    disk_reports = []
    for disk in profile.get("disk_media", []):
        source, temporary = _disk_file(disk, profile["set"], rompath, archive)
        try:
            if disk_links is not None and temporary:
                raise KonamiGVPreparationError("--link-chd requires a standalone original CHD, not a temporary archive extraction")
            report = _verify_chd(source, disk, chdman)
            filename = f"{disk['name']}.chd"
            payloads[filename] = source.read_bytes()
            if disk_links is not None:
                disk_links[filename] = source.resolve()
            disk_reports.append({**disk, "package_file": filename, "validation": report,
                                "local_sha256": hashlib.sha256(payloads[filename]).hexdigest()})
        finally:
            if temporary:
                source.unlink(missing_ok=True)
    return payloads, {"disks": disk_reports}


def _digest(data: bytes) -> tuple[str, str]:
    import zlib
    return f"{zlib.crc32(data) & 0xffffffff:08x}", hashlib.sha1(data).hexdigest()


def package_regions(set_name: str, profile: dict[str, Any], regions: dict[str, bytes],
                    output_dir: Path, source_metadata: dict[str, Any],
                    disk_payloads: dict[str, bytes] | None = None,
                    disk_reports: list[dict[str, Any]] | None = None,
                    disk_links: dict[str, Path] | None = None) -> dict[str, Any]:
    _validate_profile_shape(profile)
    canonical = next((row for row in load_registry()["profiles"] if row["set"] == set_name), None)
    if canonical is None or profile != canonical:
        raise KonamiGVPreparationError("System GV profile differs from the pinned registry")
    if profile.get("set") != set_name or profile.get("is_bios_root"):
        raise KonamiGVPreparationError("a game-specific System GV package requires a non-BIOS profile")
    if set(regions) != {row["tag"] for row in profile["rom_regions"]}:
        raise KonamiGVPreparationError("System GV assembled region tags differ from reviewed profile")
    expected_sizes = {row["tag"]: row["size"] for row in profile["rom_regions"]}
    if any(len(regions[tag]) != expected_sizes[tag] for tag in expected_sizes):
        raise KonamiGVPreparationError("System GV assembled region sizes differ from MAME")
    payloads = {REGION_FILES[tag]: data for tag, data in regions.items()}
    disk_payloads = disk_payloads or {}
    disk_reports = disk_reports or []
    disk_links = disk_links or {}
    expected_disks = {f"{disk['name']}.chd": disk for disk in profile["disk_media"]}
    if not set(disk_links).issubset(expected_disks):
        raise KonamiGVPreparationError("only declared CHDs may be linked")
    for name, source in disk_links.items():
        if source.resolve().is_relative_to(output_dir.resolve()):
            raise KonamiGVPreparationError("linked CHD must be outside the prepared output directory")
        if not source.is_file() or source.read_bytes() != disk_payloads.get(name):
            raise KonamiGVPreparationError("linked CHD differs from the verified payload")
    if any(not isinstance(report, dict) for report in disk_reports):
        raise KonamiGVPreparationError("System GV CHD report is malformed")
    if (set(disk_payloads) != {d.get("package_file") for d in disk_reports} or
            len(disk_reports) != len(profile["disk_media"])):
        raise KonamiGVPreparationError("System GV CD image set is missing, extra, or unvalidated")
    for report in disk_reports:
        if not isinstance(report, dict):
            raise KonamiGVPreparationError("System GV CHD report is malformed")
        filename = report.get("package_file")
        expected_disk = expected_disks.get(filename)
        validation = report.get("validation", {})
        if not isinstance(validation, dict):
            raise KonamiGVPreparationError("System GV CHD validation report is malformed")
        if (not expected_disk or report.get("sha1") != expected_disk["sha1"] or
                report.get("name") != expected_disk["name"] or
                type(validation.get("file_version")) is not int or
                validation.get("header_sha1") != expected_disk["sha1"] or
                validation.get("file_version") not in (3, 4, 5) or not validation.get("tracks") or
                report.get("local_sha256") != hashlib.sha256(disk_payloads[filename]).hexdigest()):
            raise KonamiGVPreparationError("System GV CHD report does not match MAME source facts")
    payloads.update(disk_payloads)
    lines = ["format=pcsx-aow-system-gv-v1", f"mame_set={set_name}",
             f"mame_parent={profile.get('parent') or 'none'}",
             f"bios_parent={profile.get('bios_parent') or 'none'}",
             f"mame_machine={profile['machine']}", f"mame_input={profile['input']}",
             f"mame_state={profile['state']}", f"mame_init={profile['init']}",
             f"mame_flags={profile['flags']}", f"cpu_device={profile['cpu_device']}",
             f"cpu_clock_hz={profile['cpu_clock_hz']}", f"gpu_device={profile['gpu_device']}",
             f"gpu_clock_hz={profile['gpu_clock_hz']}", f"main_ram_size={profile['main_ram_size']}",
             f"vram_size={profile['vram_size']}", f"scsi_device={profile['scsi_device']}",
             f"scsi_clock_hz={profile['scsi_clock_hz']}", f"cdrom_device={profile['cdrom_device']}",
             f"eeprom_device={profile['eeprom_device']}", f"duart_device={profile['duart_device']}",
             f"duart_clock_hz={profile['duart_clock_hz']}", f"spu_clock_hz={profile['spu_clock_hz']}",
             "game_execution_verified=false"]
    for region in profile["rom_regions"]:
        lines.append(f"region.{region['tag']}={REGION_FILES[region['tag']]}")
        lines.append(f"region.{region['tag']}.size={region['size']}")
    if disk_reports:
        lines.append(f"disk.cdrom={disk_reports[0]['package_file']}")
        lines.append(f"disk.cdrom.sha1={disk_reports[0]['sha1']}")
        lines.append(f"disk.cdrom.status={disk_reports[0]['status']}")
    lines.append("unsupported_game_io=" + "; ".join(profile["unsupported_game_io"]))
    lines.append("")
    board_ini = "\n".join(lines)
    manifest = {"system": "Konami System GV", "mame_driver": DRIVER,
                "mame_source_commit": boards.COMMIT, "mame_set": set_name,
                "profile": {key: profile[key] for key in (
                    "year", "parent", "machine", "input", "state", "init", "flags",
                    "source_class", "cpu_device", "cpu_clock_hz", "gpu_device", "gpu_clock_hz",
                    "main_ram_size", "vram_size", "unsupported_game_io", "game_execution_verified")},
                "source_media": source_metadata, "disk_media": disk_reports,
                "regions": {}, "game_execution_verified": False}
    for name, data in payloads.items():
        crc, sha = _digest(data)
        manifest["regions"][name] = {"size": len(data), "crc32": crc, "sha1": sha}
    if disk_links:
        manifest["disk_storage"] = {name: {"mode": "symlink", "source": str(path.resolve())}
                                    for name, path in disk_links.items()}

    output_dir.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="prepare-system-gv-", dir=output_dir.parent) as temp:
        stage = Path(temp) / "package"
        stage.mkdir()
        if output_dir.exists():
            if not output_dir.is_dir():
                raise KonamiGVPreparationError("output path exists and is not a directory")
            # A linked image is about to be revalidated/replaced. Do not
            # materialize its old target while copying unrelated user files.
            shutil.copytree(output_dir, stage, dirs_exist_ok=True,
                            ignore=lambda directory, names: set(disk_links) & set(names)
                            if Path(directory) == output_dir else set())
            old_manifest = stage / "manifest.json"
            if old_manifest.exists() and not old_manifest.is_file():
                raise KonamiGVPreparationError("existing output manifest is not a regular file")
            if old_manifest.is_file():
                _, old_owned_files = _validated_old_manifest(old_manifest)
                for name in old_owned_files:
                    (stage / name).unlink(missing_ok=True)
        for name, data in payloads.items():
            if name in disk_links:
                (stage / name).symlink_to(disk_links[name].resolve())
            else:
                (stage / name).write_bytes(data)
        (stage / "board.ini").write_text(board_ini, encoding="utf-8")
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        backup = Path(temp) / "previous"
        had_output = output_dir.exists()
        try:
            if had_output:
                os.replace(output_dir, backup)
            os.replace(stage, output_dir)
        except OSError:
            if had_output and backup.exists() and not output_dir.exists():
                os.replace(backup, output_dir)
            raise
    return manifest


def prepare(set_name: str, source: Path, rompath: Path, output_dir: Path,
            archive: Path | None = None, profiles_path: Path = boards.DEFAULT_OUTPUT,
            chdman: str | None = None, link_chd: bool = False) -> dict[str, Any]:
    registry = load_registry(profiles_path)
    if _source_commit(source) != registry["mame_source_commit"]:
        raise KonamiGVPreparationError("MAME checkout differs from the pinned registry")
    by_set = {row["set"]: row for row in registry["profiles"]}
    if set_name not in by_set:
        raise KonamiGVPreparationError(f"no System GV profile for {set_name}")
    profile = by_set[set_name]
    if profile["is_bios_root"]:
        raise KonamiGVPreparationError("konamigv is a BIOS root, not a game package")
    if profile.get("known_media_issues") and any("required NO_DUMP" in issue for issue in profile["known_media_issues"]):
        raise KonamiGVPreparationError("set requires NO_DUMP ROM media; no substitute is permitted")
    fresh = boards.build_registry(source, boards.INVENTORY)
    if fresh != registry:
        raise KonamiGVPreparationError("registry differs from pinned source/listxml facts")
    regions, source_metadata = _load_regions(set_name, source, rompath, archive)
    disk_links = {} if link_chd else None
    payloads, chd_metadata = _payloads(profile, regions, rompath, archive, chdman, disk_links)
    disk_names = {f"{d['name']}.chd" for d in profile["disk_media"]}
    disk_payloads = {name: payloads.pop(name) for name in disk_names}
    return package_regions(set_name, profile, regions, output_dir,
                           source_metadata | chd_metadata, disk_payloads,
                           chd_metadata["disks"], disk_links)


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--set", required=True, dest="set_name")
    parser.add_argument("--mame-source", required=True, type=Path)
    parser.add_argument("--rompath", required=True, type=Path)
    parser.add_argument("--archive", type=Path, help="optional local ZIP containing ROM or CHD files")
    parser.add_argument("--profiles", type=Path, default=boards.DEFAULT_OUTPUT)
    parser.add_argument("--chdman", help="path to chdman (defaults to PATH lookup)")
    parser.add_argument("--link-chd", action="store_true",
                        help="reference the audited standalone original CD instead of copying it")
    parser.add_argument("--output-dir", type=Path, default=Path("artifacts/system-gv"))
    args = parser.parse_args(argv)
    try:
        manifest = prepare(args.set_name, args.mame_source, args.rompath,
                           args.output_dir, args.archive, args.profiles, args.chdman, args.link_chd)
    except (KonamiGVPreparationError, boards.KonamiGVProfileError,
            mame.PreparationError, OSError) as exc:
        print(f"prepare_konamigv_board: {exc}", file=sys.stderr)
        return 2
    print(f"prepared {args.set_name}: {len(manifest['regions'])} verified media files; game execution remains unverified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
