#!/usr/bin/env python3
"""Embed the System 10, System 11 and Capcom ZN MAME romsets for the native zip loader.

Reads the ROM declarations of namco/namcos10.cpp, namco/namcos11.cpp and
sony/zn.cpp at the pinned MAME commit (through prepare_mame_psx.py's parser)
and the board registries, and writes libpcsxcore/arcade/arcade_romsets.inc.
For each loadable set it holds the ROM files (size, CRC32), the board.ini
text and the files of the board package that the prepare_system10_board.py,
prepare_system11_board.py and prepare_capcom_zn_board.py scripts write, each
with the ROM_LOAD/ROM_FILL operations that build it. The console then builds
the package from MAME zips without a preparation step.

The sets are the ones those scripts accept: System 10 MEM(M)/MEM(N) sets with
no unsupported hardware, System 11 sets that are not MACHINE_NOT_WORKING and
have no CHD, and the Capcom ZN sets the registry marks runtime_candidate.

  generate_arcade_romsets.py --mame-source ../mamedev
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
from dataclasses import dataclass, replace
from typing import Any, Iterator

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import capcom_zn_boards as zn_profiles  # noqa: E402
import prepare_capcom_zn_board as zn_prepare  # noqa: E402
import prepare_mame_psx as mame  # noqa: E402
import prepare_system10_board as s10_prepare  # noqa: E402
import prepare_system11_board as s11_prepare  # noqa: E402
import system10_boards as s10_boards  # noqa: E402

OUTPUT = ROOT / "libpcsxcore/arcade/arcade_romsets.inc"
SYSTEM_NAMES = {"system10": "Namco System 10", "system11": "Namco System 11", "capcom_zn": "Capcom ZN"}

# The package files of a Capcom ZN game, from region tags (prepare_capcom_zn_board.OUTPUTS).
ZN_FILES = zn_prepare.OUTPUTS
S11_FILES = {"maincpu:rom": "main.bin", "bankedroms": "banked.bin", "c76": "c76.bin", "c352": "wave.bin"}


def c_string(value: str) -> str:
    return json.dumps(value)


@dataclass
class SetSpec:
    """A loadable set and the board package files that are built from it.

    regions: (package file, region tag, region, size) for the set's own regions.
    extra: (package file, size, operations, romset) for files that other MAME sets
    declare (the C76 BIOS, the QSound DSP firmware).
    inis: the board.ini text; a set with a choice of BIOS has one for each choice, by name.
    """
    family: str
    profile: dict[str, Any]
    description: str
    inis: dict[str | None, str]
    romset: mame.RomSet
    regions: list[tuple[str, str, mame.Region | None, int]]
    extra: list[tuple[str, int, list[Any], mame.RomSet]]
    selected_bios: str | None = None


class Builder:
    """The tables of the .inc: one files/ops/vfiles slice per set."""

    def __init__(self) -> None:
        self.files: list[str] = []
        self.ops: list[str] = []
        self.vfiles: list[str] = []
        self.sets: list[str] = []
        self.set_count = 0

    def add_set(self, spec: SetSpec) -> None:
        family, profile, description = spec.family, spec.profile, spec.description
        romset, regions, extra = spec.romset, spec.regions, spec.extra
        first_file = len(self.files)
        # A choice of BIOS: its files are tagged with the choice (1-based), the others with 0.
        variants = list(romset.bios.values())
        selected = {f.index for f in romset.files}
        local: dict[tuple[str, int], int] = {}

        def file_slot(owner: mame.RomSet, index: int) -> int:
            spec = next(spec for spec in owner.files if spec.index == index)
            if spec.crc32 is None or spec.sha1 is None or spec.status == "no_dump":
                raise SystemExit(f"{profile['set']}: {spec.filename} has no dump")
            key = (owner.name, index)
            if key not in local:
                local[key] = len(self.files) - first_file
                bios = variants.index(owner.bios[spec.bios_id]) + 1 if owner is romset and spec.bios_id is not None else 0
                self.files.append(f"    {{{c_string(spec.filename)}, 0x{spec.expected_size:x}, 0x{spec.crc32}, {bios}}},")
            return local[key]

        def emit(owner: mame.RomSet, ops: list[Any], keep: set[int], limit: int | None = None) -> int:
            first = len(self.ops)
            for op in ops:
                if isinstance(op, mame.LoadOp):
                    if op.file_index not in keep:
                        continue
                    if limit is not None and op.offset + (op.length // op.group - 1) * op.stride + op.group > limit:
                        raise SystemExit(f"{profile['set']}: ROM_LOAD exceeds the {limit:#x} bytes of its package file")
                    slot = file_slot(owner, op.file_index)
                    self.ops.append(f"    {{A_LOAD, {op.group}, {op.stride}, {1 if op.swap else 0}, 0, {slot}, "
                                    f"0x{op.source_offset:x}, 0x{op.offset:x}, 0x{op.length:x}}},")
                elif isinstance(op, mame.FillOp):
                    self.ops.append(f"    {{A_FILL, 1, 1, 0, 0x{op.value:02x}, 0, 0, 0x{op.offset:x}, 0x{op.length:x}}},")
                else:
                    raise SystemExit(f"{profile['set']}: unsupported ROM operation {type(op).__name__}")
            return len(self.ops) - first

        first_vfile = len(self.vfiles)
        for name, ini in spec.inis.items():
            bios = variants.index(name) + 1 if name is not None else 0
            self.vfiles.append(f"    {{\"board.ini\", {c_string(ini)}, {len(ini.encode())}, 0, 0, 0, {bios}}},")
        for name, tag, region, size in regions:
            first_op = len(self.ops)
            count = emit(romset, region.operations, selected) if region is not None else 0
            erase = region.erase if region is not None else 0
            self.vfiles.append(f"    {{{c_string(name)}, NULL, 0x{size:x}, 0x{erase:02x}, {first_op}, {count}, 0}},")
        for name, size, ops, owner in extra:
            first_op = len(self.ops)
            count = emit(owner, ops, {spec.index for spec in owner.files}, limit=size)
            self.vfiles.append(f"    {{{c_string(name)}, NULL, 0x{size:x}, 0, {first_op}, {count}, 0}},")
        first_set_file = first_file
        info = "{" + ", ".join([c_string(SYSTEM_NAMES[family]), c_string(profile["set"]),
                                c_string(profile.get("parent") or ""), c_string(description),
                                str(int(profile["year"]))]) + "}"
        default = variants.index(romset.default_bios) + 1 if variants else 0
        self.sets.append("    {" + ", ".join([
            info, str(first_set_file), str(len(self.files) - first_set_file),
            str(first_vfile), str(len(self.vfiles) - first_vfile),
            c_string(",".join(variants)), str(default),
        ]) + "},")
        self.set_count += 1

    def render(self, commit: str) -> str:
        return "\n".join([
            f"/* Generated by scripts/generate_arcade_romsets.py from MAME {commit[:8]} "
            "(namco/namcos10.cpp, namco/namcos11.cpp, sony/zn.cpp) and the board registries in scripts/; do not edit. */",
            "static const arcade_rom_file arcade_files[] = {", *self.files, "};",
            "static const arcade_rom_op arcade_ops[] = {", *self.ops, "};",
            "static const arcade_vfile arcade_vfiles[] = {", *self.vfiles, "};",
            "static const arcade_romset arcade_sets[] = {", *self.sets, "};",
            "",
        ])


def parse(source: Path, driver: str, transform=None):
    text = (source / "src/mame" / driver).read_text(encoding="utf-8")
    if transform:
        text = transform(text)
    parsed, parents, _ = mame.parse_driver(text, driver)
    return parsed, parents


def single_file_set(source: Path, driver: str, set_name: str, region: str, transform=None) -> tuple[mame.RomSet, mame.Region]:
    parsed, _ = parse(source, driver, transform)
    romset = parsed[set_name]
    return romset, romset.regions[region]


def system10(source: Path, descriptions: dict[str, str]) -> Iterator[SetSpec]:
    commit, profiles = s10_prepare.load_profiles()
    parsed, parents = parse(source, s10_boards.DRIVER)
    for name, profile in profiles.items():
        if profile.get("unsupported_requirements") or profile["board_type"] not in ("MEM(M)", "MEM(N)") \
                or not profile.get("crypto_profile"):
            continue
        romset = parsed[name]
        if (parents.get(name) or None) != (profile["parent"] or None):
            raise SystemExit(f"{name}: parent differs between MAME and the registry")
        if any(spec.crc32 is None for spec in romset.files):
            continue
        sizes = {entry["tag"]: entry["size"] for entry in profile["rom_regions"]
                 if entry["tag"] in ("maincpu:rom", "nand", "data") or entry["tag"].startswith("nand")}
        if {tag: region.size for tag, region in romset.regions.items()} != sizes:
            raise SystemExit(f"{name}: assembled region layout differs from the pinned profile")
        names = s10_prepare.region_files(profile, romset.regions)
        ini = s10_prepare.board_ini(name, profile, names, {tag: sizes[tag] for tag in names})
        yield SetSpec("system10", profile, descriptions[name], {None: ini}, romset,
                      [(filename, tag, romset.regions[tag], sizes[tag]) for tag, filename in names.items()], [])


def system11(source: Path, descriptions: dict[str, str]) -> Iterator[SetSpec]:
    _, profiles = s11_prepare.load_profiles()
    parsed, parents = parse(source, s11_prepare.DRIVER)
    text = (source / "src/mame" / s11_prepare.MCU_DRIVER).read_text(encoding="utf-8")
    bios_sets, _, _ = mame.parse_driver(text.replace("M37710_INTERNAL_ROM_REGION", '"internal"'),
                                        s11_prepare.MCU_DRIVER)
    bios = bios_sets["c76"]
    expected = s11_prepare.EXPECTED_C76
    spec = bios.files[0]
    if (len(bios.files) != 1 or spec.filename != expected["filename"] or spec.expected_size != expected["size"]
            or spec.crc32 != expected["crc32"] or spec.sha1 != expected["sha1"]):
        raise SystemExit("C76 BIOS declaration differs from the pinned size/CRC/SHA1")
    for name, profile in profiles.items():
        if profile.get("flags") == "MACHINE_NOT_WORKING" or profile.get("status") == "not_working":
            continue
        romset = parsed[name]
        if (parents.get(name) or None) != (profile["parent"] or None):
            raise SystemExit(f"{name}: parent differs between MAME and the registry")
        if romset.disks or any(spec.crc32 is None for spec in romset.files):
            continue
        for required in ("maincpu:rom", "c76", "c352"):
            if required not in romset.regions:
                raise SystemExit(f"{name}: missing region {required}")
        if profile["banking"] != "NONE" and "bankedroms" not in romset.regions:
            raise SystemExit(f"{name}: banking {profile['banking']} without a bankedroms region")
        regions = []
        for tag, filename in S11_FILES.items():
            region = romset.regions.get(tag)
            regions.append((filename, tag, region, region.size if region else 0))
        internal = bios.regions["internal"]
        yield SetSpec("system11", profile, descriptions[name], {None: s11_prepare.board_ini(name, profile)}, romset,
                      regions, [("c76bios.bin", expected["size"], internal.operations, bios)])


def capcom_zn(source: Path, descriptions: dict[str, str]) -> Iterator[SetSpec]:
    registry = zn_prepare.load_registry()
    parsed, parents = parse(source, zn_profiles.DRIVER)
    qprofile = zn_profiles._qsound_profile(source)
    clean = mame._without_comments((source / "src" / zn_profiles.QSOUND_DRIVER).read_text(encoding="utf-8"))
    if clean.count("ROM_IGNORE( 0x4000 )") != 1:
        raise SystemExit("QSound ROM_IGNORE declaration differs from the reviewed source")
    qsets, _, _ = mame.parse_driver(clean.replace("ROM_IGNORE( 0x4000 )", ""), zn_profiles.QSOUND_DRIVER)
    qrom = qsets[qprofile["set"]]
    qspec = qrom.files[0]
    if (len(qrom.files) != 1 or len(qrom.regions) != 1 or qspec.filename != qprofile["filename"]
            or qspec.expected_size != qprofile["size"] or qspec.crc32 != qprofile["crc32"]
            or qspec.sha1 != qprofile["sha1"]):
        raise SystemExit("QSound DSP ROM_LOAD differs from the pinned MAME declaration")
    # MAME's rom_file_size() adds ROM_IGNORE's 0x4000 to the file length.
    qrom.files[0] = replace(qspec, expected_size=qprofile["file_size"])
    dsp = qrom.regions[qprofile["region"]]
    if dsp.size != zn_profiles.EXPECTED_QSOUND_DSP["size"]:
        raise SystemExit("QSound DSP region has the wrong size")
    for profile in registry["profiles"]:
        name = profile["set"]
        if not profile.get("runtime_candidate"):
            continue
        romset = parsed[name]
        if romset.disks or romset.source_driver != zn_profiles.DRIVER:
            raise SystemExit(f"{name}: CHD media is unsupported")
        if set(romset.regions) != set(profile["required_regions"] + profile["optional_undumped_regions"]):
            raise SystemExit(f"{name}: ROM regions differ from the reviewed profile")
        romset = zn_prepare._filter_reviewed_unused_mcu(romset, profile)
        sizes = {entry["tag"]: entry["size"] for entry in profile["rom_regions"] if entry["tag"] in ZN_FILES}
        if {tag: romset.regions[tag].size for tag in ZN_FILES} != sizes:
            raise SystemExit(f"{name}: assembled region geometry differs from the pinned profile")
        regions = [(filename, tag, romset.regions[tag], sizes[tag]) for tag, filename in ZN_FILES.items()]
        inis = ({bios: zn_prepare.board_ini(name, profile, bios) for bios in romset.bios.values()}
                or {None: zn_prepare.board_ini(name, profile, None)})
        yield SetSpec("capcom_zn", profile, descriptions[name], inis,
                      romset, regions, [("qsound_dsp.bin", dsp.size, dsp.operations, qrom)], romset.default_bios)


def writes_cleanly(spec: SetSpec) -> str | None:
    """None if the set's ROM_LOAD/ROM_FILL operations assemble as
    prepare_mame_psx.assemble_regions does, for each choice of BIOS (inside each
    region, no byte written twice but by a ROM_CONTINUE); otherwise the reason
    it would not. The zip loader then loads exactly the sets that the preparers
    can package."""
    romset = spec.romset
    choices = list(romset.bios.values()) or [None]
    for bios in choices:
        selected = {f.index for f in romset.files
                    if f.bios_id is None or bios is None or romset.bios.get(f.bios_id) == bios}
        owners = [(romset, selected)] + [(owner, {f.index for f in owner.files}) for _, _, _, owner in spec.extra]
        for owner, keep in owners:
            for tag, region in owner.regions.items():
                written = bytearray(region.size)
                for op in region.operations:
                    if isinstance(op, mame.LoadOp):
                        if op.file_index not in keep:
                            continue
                        lanes, units, stride, again = op.group, op.length // op.group, op.stride, op.macro == "ROM_CONTINUE"
                    else:
                        lanes, units, stride, again = 1, op.length, 1, False
                        op = mame.LoadOp(0, 0, op.offset, op.length, 1, 1, False, "")
                    if not units:
                        continue
                    for lane in range(lanes):
                        start = op.offset + lane
                        end = start + (units - 1) * stride + 1
                        if end > len(written):
                            return f"a ROM write exceeds region {tag}"
                        if not again and written[start:end:stride].count(1):
                            return f"overlapping ROM writes in region {tag} at {start:#x}"
                        written[start:end:stride] = b"\x01" * units
    return None


def loadable_sets(source: Path) -> Iterator[SetSpec]:
    """The sets of the three families that the prepare_*.py scripts accept."""
    commit = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"], check=True,
                            capture_output=True, text=True).stdout.strip()
    if commit != mame.PINNED_MAME_COMMIT:
        raise SystemExit(f"MAME source is at {commit}, not the pinned {mame.PINNED_MAME_COMMIT}")
    inventory = json.loads((HERE / "mame_psx_systems.json").read_text())
    descriptions = {row["set"]: row["description"] for system in inventory["systems"] for row in system["sets"]}
    for family in (system10, system11, capcom_zn):
        for spec in family(source, descriptions):
            reason = writes_cleanly(spec)
            if reason:
                print(f"skipped {spec.profile['set']}: {reason} (the preparer rejects it too)", file=sys.stderr)
                continue
            yield spec


def generate(source: Path) -> tuple[str, dict[str, int]]:
    builder = Builder()
    counts: dict[str, int] = {}
    for spec in loadable_sets(source):
        builder.add_set(spec)
        counts[spec.family] = counts.get(spec.family, 0) + 1
    return builder.render(mame.PINNED_MAME_COMMIT), counts


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--mame-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--check", action="store_true", help="fail if the output file is stale")
    args = parser.parse_args()
    text, counts = generate(args.mame_source)
    if args.check:
        if not args.output.is_file() or args.output.read_text(encoding="utf-8") != text:
            raise SystemExit(f"{args.output} is stale; run scripts/generate_arcade_romsets.py")
    else:
        args.output.write_text(text, encoding="utf-8")
    print(", ".join(f"{count} {family}" for family, count in counts.items()) + " sets")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
