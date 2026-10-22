#!/usr/bin/env python3
"""Verify user-supplied MAME ROM media and assemble declared PSX regions.

This emits raw MAME memory regions, not a configured or verified arcade board.
It performs no network access and does not fetch ROMs or CHDs.
"""
from __future__ import annotations

import argparse
import ast
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable

PINNED_MAME_COMMIT = "deb2f54fe7300e7a192690318beb860159bfd52a"
DRIVERS = (
    "sony/psx.cpp", "sony/zn.cpp", "sony/taitogn.cpp",
    "konami/konamigv.cpp", "konami/konamigq.cpp", "konami/ksys573.cpp",
    "konami/twinkle.cpp", "namco/namcos10.cpp", "namco/namcos11.cpp",
    "namco/namcos12.cpp",
)
ROM_START_RE = re.compile(r"\bROM_START\s*\(")
ROM_CALL_RE = re.compile(r"\b(ROMX?_[A-Z0-9_]+|DISK_[A-Z0-9_]+)\s*\(")
CRC_RE = re.compile(r"\bCRC\s*\(\s*([0-9a-fA-F]+)\s*\)")
SHA1_RE = re.compile(r"\bSHA1\s*\(\s*([0-9a-fA-F]{40})\s*\)")
BIOS_RE = re.compile(r"\bROM_BIOS\s*\(\s*([^()]*)\s*\)")


class PreparationError(Exception):
    """A source declaration, input archive, or output layout is invalid."""


@dataclass
class Macro:
    params: list[str] | None
    body: str


@dataclass
class Region:
    tag: str
    size: int
    width: int
    endian: str
    flags: str
    erase: int
    operations: list[Any] = field(default_factory=list)


@dataclass(frozen=True)
class FileSpec:
    index: int
    filename: str
    expected_size: int
    crc32: str | None
    sha1: str | None
    status: str
    bios_id: str | None


@dataclass(frozen=True)
class LoadOp:
    file_index: int
    source_offset: int
    offset: int
    length: int
    group: int
    stride: int
    swap: bool
    macro: str


@dataclass(frozen=True)
class FillOp:
    offset: int
    length: int
    value: int


@dataclass(frozen=True)
class CopyOp:
    source_region: str
    source_offset: int
    offset: int
    length: int


@dataclass(frozen=True)
class DiskImage:
    name: str
    region: str
    index: int
    sha1: str | None
    readonly: bool


@dataclass
class RomSet:
    name: str
    regions: dict[str, Region]
    files: list[FileSpec]
    loads: list[LoadOp]
    disks: list[DiskImage]
    bios: dict[str, str]
    default_bios: str | None
    source_driver: str


def _without_comments(text: str) -> str:
    # Preserve newlines and quoted strings as MAME filenames may contain //.
    out: list[str] = []
    i = 0
    quoted = escaped = False
    while i < len(text):
        if quoted:
            char = text[i]
            out.append(char)
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
            i += 1
        elif text[i] == '"':
            quoted = True
            out.append(text[i])
            i += 1
        elif text.startswith("//", i):
            end = text.find("\n", i)
            if end < 0:
                break
            out.append("\n")
            i = end + 1
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                raise PreparationError("unterminated C comment in MAME source")
            out.extend("\n" for char in text[i:end + 2] if char == "\n")
            i = end + 2
        else:
            out.append(text[i])
            i += 1
    if quoted:
        raise PreparationError("unterminated string in MAME source")
    return "".join(out)


def _call_arguments(text: str, open_paren: int) -> tuple[str, int]:
    depth = 1
    quoted = escaped = False
    for index in range(open_paren + 1, len(text)):
        char = text[index]
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren + 1:index], index + 1
    raise PreparationError("unterminated declaration call")


def _split_arguments(args: str) -> list[str]:
    parts: list[str] = []
    depth = 0
    quoted = escaped = False
    start = 0
    for index, char in enumerate(args):
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        elif char == "," and depth == 0:
            parts.append(args[start:index].strip())
            start = index + 1
    parts.append(args[start:].strip())
    return parts


def _macros(source: str) -> tuple[dict[str, Macro], str]:
    lines = source.splitlines()
    definitions: dict[str, Macro] = {}
    output: list[str] = []
    i = 0
    define_re = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)(\([^\n]*?\))?\s*(.*)$")
    while i < len(lines):
        match = define_re.match(lines[i])
        if not match:
            output.append(lines[i])
            i += 1
            continue
        name, raw_params, body = match.groups()
        chunks = [body]
        while chunks[-1].rstrip().endswith("\\"):
            chunks[-1] = chunks[-1].rstrip()[:-1]
            i += 1
            if i >= len(lines):
                raise PreparationError(f"unterminated macro definition: {name}")
            chunks.append(lines[i])
        params = None
        if raw_params is not None:
            params = [item.strip() for item in raw_params[1:-1].split(",") if item.strip()]
        definitions[name] = Macro(params, "\n".join(chunks))
        i += 1
    return definitions, "\n".join(output)


def _expand_macros(text: str, macros: dict[str, Macro], depth: int = 0) -> str:
    if depth > 24:
        raise PreparationError("ROM helper macro expansion exceeded 24 levels")
    changed = False
    output: list[str] = []
    position = 0
    token_re = re.compile(r"\b[A-Za-z_]\w*\b")
    while True:
        match = token_re.search(text, position)
        if not match:
            output.append(text[position:])
            break
        output.append(text[position:match.start()])
        name = match.group(0)
        macro = macros.get(name)
        after_name = match.end()
        if macro is None:
            output.append(name)
            position = after_name
            continue
        if macro.params is None:
            replacement = macro.body
        else:
            open_index = after_name
            while open_index < len(text) and text[open_index].isspace():
                open_index += 1
            if open_index >= len(text) or text[open_index] != "(": 
                output.append(name)
                position = after_name
                continue
            raw_args, end = _call_arguments(text, open_index)
            args = _split_arguments(raw_args)
            if len(args) != len(macro.params):
                raise PreparationError(f"macro {name} expects {len(macro.params)} arguments, got {len(args)}")
            replacement = macro.body
            for param, value in zip(macro.params, args):
                replacement = re.sub(rf"\b{re.escape(param)}\b", value, replacement)
            position = end
        if macro.params is None:
            position = after_name
        output.append(replacement)
        changed = True
    result = "".join(output)
    if changed:
        return _expand_macros(result, macros, depth + 1)
    return result


def _safe_int(expr: str) -> int:
    """Evaluate an integer constant expression without executing source code."""
    expr = re.sub(r"(?<=\d)[uUlL]+\b", "", expr.strip())
    try:
        node = ast.parse(expr, mode="eval").body
    except SyntaxError as exc:
        raise PreparationError(f"unsupported integer expression: {expr}") from exc

    def visit(item: ast.AST) -> int:
        if isinstance(item, ast.Constant) and isinstance(item.value, int):
            return item.value
        if isinstance(item, ast.UnaryOp) and isinstance(item.op, (ast.UAdd, ast.USub, ast.Invert)):
            value = visit(item.operand)
            return value if isinstance(item.op, ast.UAdd) else -value if isinstance(item.op, ast.USub) else ~value
        if isinstance(item, ast.BinOp) and isinstance(item.op, (ast.Add, ast.Sub, ast.Mult, ast.FloorDiv, ast.Div, ast.LShift, ast.RShift, ast.BitOr, ast.BitAnd)):
            left, right = visit(item.left), visit(item.right)
            if isinstance(item.op, ast.Add): return left + right
            if isinstance(item.op, ast.Sub): return left - right
            if isinstance(item.op, ast.Mult): return left * right
            if isinstance(item.op, (ast.FloorDiv, ast.Div)): return left // right
            if isinstance(item.op, ast.LShift): return left << right
            if isinstance(item.op, ast.RShift): return left >> right
            if isinstance(item.op, ast.BitOr): return left | right
            return left & right
        raise PreparationError(f"unsupported integer expression: {expr}")
    return visit(node)


def _string_arg(text: str) -> str:
    match = re.fullmatch(r'\s*"((?:[^"\\]|\\.)*)"\s*', text)
    if not match:
        raise PreparationError(f"expected a quoted string, got {text!r}")
    return bytes(match.group(1), "utf-8").decode("unicode_escape")


def _load_mode(macro: str) -> tuple[int, int, bool]:
    mode = macro.removeprefix("ROM_LOAD").removeprefix("ROMX_LOAD")
    table = {
        "": (1, 1, False),
        "16_WORD": (2, 2, False),
        "16_WORD_SWAP": (2, 2, True),
        "16_BYTE": (1, 2, False),
        "32_WORD": (2, 4, False),
        "32_WORD_SWAP": (2, 4, True),
        "32_BYTE": (1, 4, False),
    }
    if mode not in table:
        raise PreparationError(f"unsupported ROM load layout {macro}")
    return table[mode]


def _parse_load(args: list[str], macro: str, file_index: int,
                source_offset: int = 0) -> tuple[FileSpec, LoadOp]:
    minimum = 5 if macro.startswith("ROMX_LOAD") else 4
    if len(args) < minimum:
        raise PreparationError(f"malformed {macro} declaration")
    filename = _string_arg(args[0])
    offset, length = _safe_int(args[1]), _safe_int(args[2])
    digest_text = args[3]
    if re.search(r"\bNO_DUMP\b", digest_text):
        spec = FileSpec(file_index, filename, length, None, None, "no_dump", None)
    else:
        crc = CRC_RE.search(digest_text)
        sha = SHA1_RE.search(digest_text)
        if not crc or not sha:
            raise PreparationError(f"load {filename} requires CRC and SHA1 (or NO_DUMP)")
        status = "bad_dump" if re.search(r"\bBAD_DUMP\b", digest_text) else "good"
        bios = BIOS_RE.search(digest_text + " " + " ".join(args[4:]))
        bios_id = bios.group(1).strip() if bios else None
        spec = FileSpec(file_index, filename, length, crc.group(1).lower().zfill(8),
                        sha.group(1).lower(), status, bios_id)
    group, stride, swap = _load_mode(macro)
    return spec, LoadOp(file_index, source_offset, offset, length, group, stride, swap, macro)


def _region_params(macro: str, args: list[str]) -> tuple[int, str, int, str, str, int]:
    if len(args) < 2:
        raise PreparationError(f"malformed {macro} declaration")
    size = _safe_int(args[0])
    tag = _string_arg(args[1])
    suffix = macro.removeprefix("ROM_REGION")
    width = int(suffix[:2]) if suffix[:2] in ("16", "32") else 8
    endian = "little" if suffix.endswith("_LE") else "big" if suffix.endswith("_BE") else "native"
    flags = args[2] if len(args) > 2 else "0"
    if "ROMREGION_BE" in flags: endian = "big"
    if "ROMREGION_LE" in flags: endian = "little"
    if "ROMREGION_16BIT" in flags: width = 16
    if "ROMREGION_32BIT" in flags: width = 32
    erase = 0xff if "ROMREGION_ERASEFF" in flags else 0x00
    erase_match = re.search(r"ROMREGION_ERASEVAL\s*\(\s*([^()]+)\s*\)", flags)
    if erase_match:
        erase = _safe_int(erase_match.group(1))
    if size <= 0:
        raise PreparationError(f"invalid region size {size} for {tag}")
    if erase < 0 or erase > 255:
        raise PreparationError(f"invalid erase byte {erase} for {tag}")
    return size, tag, width, endian, flags, erase


def _parse_block(name: str, block: str, macros: dict[str, Macro], driver: str) -> RomSet:
    block = _expand_macros(block, macros)
    regions: dict[str, Region] = {}
    files: list[FileSpec] = []
    loads: list[LoadOp] = []
    disks: list[DiskImage] = []
    bios: dict[str, str] = {}
    default_bios: str | None = None
    current: Region | None = None
    current_disk = ""
    current_file: dict[str, Any] | None = None
    position = 0
    while True:
        match = ROM_CALL_RE.search(block, position)
        if not match:
            break
        macro = match.group(1)
        args_text, end = _call_arguments(block, match.end() - 1)
        args = _split_arguments(args_text)
        position = end
        if macro.startswith("ROM_REGION"):
            size, tag, width, endian, flags, erase = _region_params(macro, args)
            if tag in regions:
                raise PreparationError(f"duplicate region {tag} in {name}")
            current = Region(tag, size, width, endian, flags, erase)
            regions[tag] = current
            current_file = None
        elif macro == "ROM_SYSTEM_BIOS":
            if len(args) < 3:
                raise PreparationError(f"malformed ROM_SYSTEM_BIOS in {name}")
            bios[str(_safe_int(args[0]))] = _string_arg(args[1])
        elif macro == "ROM_DEFAULT_BIOS":
            if not args:
                raise PreparationError(f"malformed ROM_DEFAULT_BIOS in {name}")
            default_bios = _string_arg(args[0])
        elif macro.startswith("ROM_LOAD") or macro.startswith("ROMX_LOAD"):
            if current is None:
                raise PreparationError(f"load without region in {name}")
            index = len(files)
            spec, operation = _parse_load(args, macro, index)
            files.append(spec)
            loads.append(operation)
            current.operations.append(operation)
            current_file = {"index": index, "cursor": operation.length}
        elif macro == "ROM_CONTINUE":
            if current is None or current_file is None or len(args) < 2:
                raise PreparationError(f"ROM_CONTINUE without a preceding load in {name}")
            offset, length = _safe_int(args[0]), _safe_int(args[1])
            spec = files[current_file["index"]]
            mode_macro = next(op.macro for op in reversed(loads) if op.file_index == current_file["index"])
            group, stride, swap = _load_mode(mode_macro)
            operation = LoadOp(spec.index, current_file["cursor"], offset, length,
                               group, stride, swap, "ROM_CONTINUE")
            current.operations.append(operation)
            loads.append(operation)
            current_file["cursor"] += length
            files[spec.index] = FileSpec(spec.index, spec.filename, current_file["cursor"],
                                         spec.crc32, spec.sha1, spec.status, spec.bios_id)
        elif macro == "ROM_RELOAD":
            if current is None or current_file is None or len(args) < 2:
                raise PreparationError(f"ROM_RELOAD without a preceding load in {name}")
            offset, length = _safe_int(args[0]), _safe_int(args[1])
            spec = files[current_file["index"]]
            previous = next(op for op in reversed(loads) if op.file_index == spec.index)
            operation = LoadOp(spec.index, 0, offset, length, previous.group,
                               previous.stride, previous.swap, "ROM_RELOAD")
            current.operations.append(operation)
            loads.append(operation)
        elif macro == "ROM_FILL":
            if current is None or len(args) < 3:
                raise PreparationError(f"malformed ROM_FILL in {name}")
            current.operations.append(FillOp(_safe_int(args[0]), _safe_int(args[1]), _safe_int(args[2])))
        elif macro == "ROM_COPY":
            if current is None or len(args) < 4:
                raise PreparationError(f"malformed ROM_COPY in {name}")
            current.operations.append(CopyOp(_string_arg(args[0]), _safe_int(args[1]),
                                             _safe_int(args[2]), _safe_int(args[3])))
        elif macro == "DISK_REGION":
            if not args:
                raise PreparationError(f"malformed DISK_REGION in {name}")
            current_disk = _string_arg(args[0])
        elif macro in ("DISK_IMAGE", "DISK_IMAGE_READONLY"):
            if len(args) < 2:
                raise PreparationError(f"malformed {macro} in {name}")
            sha = SHA1_RE.search(" ".join(args[2:]))
            disks.append(DiskImage(_string_arg(args[0]), current_disk,
                                   _safe_int(args[1]), sha.group(1).lower() if sha else None,
                                   macro == "DISK_IMAGE_READONLY"))
        else:
            raise PreparationError(f"unsupported declaration {macro} in {name}")
    if not regions:
        raise PreparationError(f"no ROM regions in {name}")
    if bios:
        if default_bios is None:
            default_bios = bios[min(bios, key=lambda value: int(value))]
        if default_bios not in bios.values():
            raise PreparationError(f"default BIOS {default_bios!r} is not declared in {name}")
    return RomSet(name, regions, files, loads, disks, bios, default_bios, driver)


def parse_driver(source_text: str, driver: str) -> tuple[dict[str, RomSet], dict[str, str], list[dict[str, str]]]:
    source = _without_comments(source_text)
    macros, source_without_macros = _macros(source)
    starts = list(ROM_START_RE.finditer(source_without_macros))
    games: dict[str, str] = {}
    for game in re.finditer(r"\b(?:GAME|GAMEL|GAMEC|GAMEI|GAMES|CONS|SYST)\s*\(", source_without_macros):
        args_text, _ = _call_arguments(source_without_macros, game.end() - 1)
        args = _split_arguments(args_text)
        if len(args) >= 3:
            set_name = args[1].strip()
            parent = args[2].strip()
            if re.fullmatch(r"[A-Za-z0-9_]+", set_name) and parent not in ("0", "nullptr"):
                games[set_name] = parent
    parsed: dict[str, RomSet] = {}
    coverage: list[dict[str, str]] = []
    for index, start in enumerate(starts):
        args, begin = _call_arguments(source_without_macros, start.end() - 1)
        name_args = _split_arguments(args)
        if not name_args:
            continue
        name = name_args[0].strip()
        end_match = re.search(r"\bROM_END\b", source_without_macros[begin:])
        if not end_match:
            coverage.append({"set": name, "driver": driver, "status": "failure", "reason": "missing ROM_END"})
            continue
        end = begin + end_match.start()
        block = source_without_macros[begin:end]
        try:
            romset = _parse_block(name, block, macros, driver)
            parsed[name] = romset
            coverage.append({"set": name, "driver": driver, "status": "parsed", "reason": ""})
        except PreparationError as exc:
            coverage.append({"set": name, "driver": driver, "status": "unsupported", "reason": str(exc)})
    return parsed, games, coverage


def _hash(data: bytes) -> tuple[str, str]:
    return f"{zlib.crc32(data) & 0xffffffff:08x}", hashlib.sha1(data).hexdigest()


def _lineage(set_name: str, parents: dict[str, str]) -> list[str]:
    result: list[str] = []
    seen = {set_name}
    current = set_name
    while current in parents:
        current = parents[current]
        if current in seen:
            raise PreparationError(f"cycle in MAME parent chain at {current}")
        result.append(current)
        seen.add(current)
    return result


def _zip_files(path: Path) -> list[str]:
    if not path.exists():
        return []
    try:
        with zipfile.ZipFile(path) as archive:
            names = archive.namelist()
    except zipfile.BadZipFile as exc:
        raise PreparationError(f"invalid ROM archive {path}") from exc
    if len(names) != len(set(names)):
        raise PreparationError(f"ROM archive contains duplicate member names: {path}")
    return names


def _load_rom_files(romset: RomSet, rompath: Path, lineage: list[str],
                    selected_bios: str | None,
                    explicit_archive: Path | None = None) -> tuple[dict[int, bytes], dict[int, str]]:
    selected_specs = [spec for spec in romset.files
                      if spec.bios_id is None or selected_bios is None
                      or romset.bios.get(spec.bios_id) == selected_bios]
    if romset.bios and selected_bios is None:
        raise PreparationError(f"{romset.name} requires an explicit/default BIOS selection")
    data_by_index: dict[int, bytes] = {}
    origin_by_index: dict[int, str] = {}
    for spec in selected_specs:
        if spec.status == "no_dump" or spec.crc32 is None or spec.sha1 is None:
            raise PreparationError(f"required media is NO_DUMP: {spec.filename}; region data cannot be complete")
        if spec.expected_size <= 0:
            raise PreparationError(f"invalid expected size for {spec.filename}")
        matched: tuple[bytes, str] | None = None
        bad_candidate = False
        preferred = ([explicit_archive] if explicit_archive else []) + [
            rompath / f"{archive_name}.zip" for archive_name in [romset.name, *lineage]
        ]
        # MAME users often keep a renamed archive (e.g. a parent/merged set
        # under their preferred short name). Search other local ZIPs by the
        # declared member name, then accept only exact size/CRC/SHA1 matches.
        # Hashes, rather than archive naming, decide whether media is usable.
        preferred.extend(sorted(rompath.glob("*.zip")))
        archive_candidates = list(dict.fromkeys(path.resolve() for path in preferred))
        for archive_path in archive_candidates:
            names = _zip_files(archive_path)
            matches = [name for name in names if Path(name).name == spec.filename]
            if len(matches) > 1:
                raise PreparationError(f"ambiguous duplicate {spec.filename} in {archive_path}")
            if matches:
                with zipfile.ZipFile(archive_path) as archive:
                    data = archive.read(matches[0])
                crc, sha = _hash(data)
                if len(data) == spec.expected_size and crc == spec.crc32 and sha == spec.sha1:
                    matched = (data, str(archive_path.resolve()))
                    break
                bad_candidate = True
        if matched is None:
            loose = rompath / spec.filename
            if loose.is_file():
                data = loose.read_bytes()
                crc, sha = _hash(data)
                if len(data) == spec.expected_size and crc == spec.crc32 and sha == spec.sha1:
                    matched = (data, str(loose.resolve()))
                else:
                    bad_candidate = True
        if matched is None:
            detail = "found media failed size/CRC/SHA1 validation" if bad_candidate else "required media is missing"
            raise PreparationError(f"{detail}: {spec.filename} (expected {spec.expected_size} bytes, CRC {spec.crc32}, SHA1 {spec.sha1})")
        data_by_index[spec.index] = matched[0]
        origin_by_index[spec.index] = matched[1]
    return data_by_index, origin_by_index


def _mark_written(written: bytearray, start: int, length: int, stride: int, tag: str,
                  overlap_ok: bool = False) -> None:
    if length < 0 or start < 0 or stride <= 0:
        raise PreparationError(f"invalid write bounds in region {tag}")
    end = start + (length - 1) * stride + 1 if length else start
    if end > len(written):
        raise PreparationError(f"write exceeds region {tag}: ends at {end:#x}, size {len(written):#x}")
    selected = written[start:end:stride]
    if not overlap_ok and any(selected):
        raise PreparationError(f"overlapping ROM writes in region {tag} at {start:#x}")
    written[start:end:stride] = b"\x01" * length


def _apply_load(region: bytearray, written: bytearray, op: LoadOp, source: bytes, tag: str) -> None:
    if op.length % op.group:
        raise PreparationError(f"load length is not a multiple of its group in region {tag}: {op.macro}")
    if op.source_offset < 0 or op.source_offset + op.length > len(source):
        raise PreparationError(f"source range exceeds ROM file for {op.macro} in {tag}")
    units = op.length // op.group
    if not units:
        return
    span = (units - 1) * op.stride + op.group
    if op.offset < 0 or op.offset + span > len(region):
        raise PreparationError(f"ROM load exceeds region {tag}: {op.offset:#x}+{span:#x}")
    block = source[op.source_offset:op.source_offset + op.length]
    for lane in range(op.group):
        lane_src = block[lane::op.group]
        lane_dst = op.offset + lane
        # MAME writes a ROM_CONTINUE over what is there ("first & second half
        # identical"): the later write wins. Any other overlap is a defect.
        _mark_written(written, lane_dst, units, op.stride, tag, overlap_ok=op.macro == "ROM_CONTINUE")
        if op.swap:
            other_src = block[(op.group - 1 - lane)::op.group]
            region[lane_dst:lane_dst + units * op.stride:op.stride] = other_src
        else:
            region[lane_dst:lane_dst + units * op.stride:op.stride] = lane_src


def assemble_regions(romset: RomSet, data_by_index: dict[int, bytes],
                    selected_bios: str | None = None) -> dict[str, bytes]:
    selected_file_indices = {spec.index for spec in romset.files
                             if spec.bios_id is None or selected_bios is None
                             or romset.bios.get(spec.bios_id) == selected_bios}
    output: dict[str, bytearray] = {}
    written: dict[str, bytearray] = {}
    for tag, region in romset.regions.items():
        output[tag] = bytearray([region.erase]) * region.size
        written[tag] = bytearray(region.size)
    copies: list[tuple[str, CopyOp]] = []
    for tag, region in romset.regions.items():
        for op in region.operations:
            if isinstance(op, LoadOp):
                if op.file_index in selected_file_indices:
                    if op.file_index not in data_by_index:
                        raise PreparationError(f"no verified file data for region {tag}")
                    _apply_load(output[tag], written[tag], op, data_by_index[op.file_index], tag)
            elif isinstance(op, FillOp):
                if op.offset < 0 or op.length < 0 or op.offset + op.length > region.size:
                    raise PreparationError(f"ROM_FILL exceeds region {tag}")
                _mark_written(written[tag], op.offset, op.length, 1, tag)
                output[tag][op.offset:op.offset + op.length] = bytes([op.value]) * op.length
            elif isinstance(op, CopyOp):
                copies.append((tag, op))
            else:
                raise PreparationError(f"internal unsupported operation in region {tag}")
    for destination, op in copies:
        if op.source_region not in output:
            raise PreparationError(f"ROM_COPY source region not found: {op.source_region}")
        if (op.source_offset < 0 or op.offset < 0 or op.length < 0
                or op.source_offset + op.length > len(output[op.source_region])
                or op.offset + op.length > len(output[destination])):
            raise PreparationError(f"ROM_COPY exceeds region bounds: {op.source_region} -> {destination}")
        _mark_written(written[destination], op.offset, op.length, 1, destination)
        # Bytes are copied from the completed source region as declared by MAME.
        output[destination][op.offset:op.offset + op.length] = output[op.source_region][op.source_offset:op.source_offset + op.length]
    return {tag: bytes(region) for tag, region in output.items()}


def _selected_bios(romset: RomSet, requested: str | None) -> str | None:
    if not romset.bios:
        if requested:
            raise PreparationError(f"{romset.name} has no selectable BIOS")
        return None
    selection = requested or romset.default_bios
    if selection not in romset.bios.values():
        raise PreparationError(f"unknown BIOS {selection!r} for {romset.name}; choices: {', '.join(romset.bios.values())}")
    return selection


def _find_romset(source: Path, set_name: str) -> tuple[RomSet, dict[str, str], list[dict[str, str]]]:
    matches: list[tuple[RomSet, dict[str, str], list[dict[str, str]]]] = []
    coverage: list[dict[str, str]] = []
    for driver in DRIVERS:
        path = source / "src/mame" / driver
        if not path.is_file():
            raise PreparationError(f"MAME driver file not found: {path}")
        parsed, games, report = parse_driver(path.read_text(encoding="utf-8"), driver)
        coverage.extend(report)
        if set_name in parsed:
            matches.append((parsed[set_name], games, report))
    if len(matches) != 1:
        if not matches:
            raise PreparationError(f"ROM_START({set_name}) not found in the ten inventoried MAME drivers")
        raise PreparationError(f"ROM_START({set_name}) appears in multiple drivers")
    return matches[0][0], matches[0][1], coverage


def prepare(set_name: str, source: Path, rompath: Path, output_dir: Path,
            bios: str | None = None, coverage_path: Path | None = None,
            archive: Path | None = None) -> dict[str, Any]:
    if not source.is_dir():
        raise PreparationError(f"MAME source directory not found: {source}")
    try:
        commit = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"],
                                check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise PreparationError(f"cannot identify MAME source commit at {source}") from exc
    if commit != PINNED_MAME_COMMIT:
        raise PreparationError(f"MAME source commit {commit} differs from pinned {PINNED_MAME_COMMIT}")
    romset, parents, coverage = _find_romset(source, set_name)
    if coverage_path:
        coverage_path.parent.mkdir(parents=True, exist_ok=True)
        coverage_path.write_text(json.dumps({
            "mame_commit": commit,
            "drivers": list(DRIVERS),
            "totals": {
                "rom_start_blocks": len(coverage),
                "parsed": sum(row["status"] == "parsed" for row in coverage),
                "unsupported": sum(row["status"] == "unsupported" for row in coverage),
                "failed": sum(row["status"] == "failure" for row in coverage),
            },
            "sets": coverage,
        }, indent=2) + "\n", encoding="utf-8")
    selected_bios = _selected_bios(romset, bios)
    lineage = _lineage(set_name, parents)
    media, origins = _load_rom_files(romset, rompath, lineage, selected_bios, archive)
    regions = assemble_regions(romset, media, selected_bios)
    for disk in romset.disks:
        if not disk.sha1:
            raise PreparationError(f"disk media declaration lacks SHA1: {disk.name}")

    output_dir.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="prepare-mame-psx-") as temporary:
        stage = Path(temporary)
        manifest_regions: dict[str, Any] = {}
        for index, (tag, content) in enumerate(regions.items()):
            region = romset.regions[tag]
            safe_tag = re.sub(r"[^A-Za-z0-9_.-]+", "_", tag).strip("_") or f"region_{index}"
            filename = f"{index:03d}_{safe_tag}.bin"
            path = stage / filename
            path.write_bytes(content)
            crc, sha = _hash(content)
            manifest_regions[tag] = {
                "file": filename, "size": len(content), "crc32": crc,
                "sha1": sha, "width": region.width, "endian": region.endian,
                "flags": region.flags, "erase_value": region.erase,
            }
        manifest: dict[str, Any] = {
            "system": "MAME PlayStation-derived system",
            "mame_set": romset.name,
            "mame_driver": romset.source_driver,
            "mame_source_commit": commit,
            "parent_fallback_order": lineage,
            "selected_bios": selected_bios,
            "rom_files": [
                {"filename": spec.filename, "expected_size": spec.expected_size,
                 "crc32": spec.crc32, "sha1": spec.sha1, "status": spec.status,
                 "provided_by": origins.get(spec.index)}
                for spec in romset.files
                if spec.index in media
            ],
            "regions": manifest_regions,
            "pending_disk_media": [
                {"name": disk.name, "region": disk.region, "index": disk.index,
                 "sha1": disk.sha1, "readonly": disk.readonly,
                 "status": "required_local_chd_not_assembled"}
                for disk in romset.disks
            ],
            "rom_regions_assembled": True,
            "disk_media_complete": not bool(romset.disks),
            "board_execution_verified": False,
        }
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        if output_dir.exists():
            shutil.rmtree(output_dir)
        shutil.copytree(stage, output_dir)
    return manifest


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--set", required=True, dest="set_name", help="MAME machine short name")
    parser.add_argument("--mame-source", type=Path, required=True, help="pinned local MAME checkout")
    parser.add_argument("--rompath", type=Path, required=True, help="directory with local set/parent ZIPs or loose ROM files")
    parser.add_argument("--archive", type=Path, help="explicit local ZIP to search first, regardless of its filename")
    parser.add_argument("--output-dir", type=Path, default=Path("artifacts/mame-psx"))
    parser.add_argument("--bios", help="select a BIOS variant by MAME short name")
    parser.add_argument("--coverage-report", type=Path, help="write parse/support classification for all 491 ROM_START blocks")
    args = parser.parse_args(argv)
    try:
        manifest = prepare(args.set_name, args.mame_source, args.rompath,
                           args.output_dir, args.bios, args.coverage_report, args.archive)
    except (OSError, PreparationError, zipfile.BadZipFile) as exc:
        print(f"prepare_mame_psx: {exc}", file=sys.stderr)
        return 2
    pending = len(manifest["pending_disk_media"])
    print(f"assembled {len(manifest['regions'])} ROM regions for {args.set_name}; {pending} disk image(s) pending; board execution remains unverified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
