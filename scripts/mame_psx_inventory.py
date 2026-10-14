#!/usr/bin/env python3
"""Generate a pinned, deterministic inventory of MAME systems built around PSX SoCs.

The machine boundary is the listxml main CPU's Sony CXD85xx/CXD86xx part name.
That captures the PlayStation-derived CPU/GPU ASIC families while excluding
unrelated MIPS R3000/R3041 machines and PlayStation 2 Emotion Engine systems.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any, Iterable

PINNED_MAME_COMMIT = "deb2f54fe7300e7a192690318beb860159bfd52a"
CPU_SIGNATURE = re.compile(r"\bSony CXD8[5-9][0-9]{2}[A-Z0-9]*\b", re.I)

# Human-readable board scope and known RAM configuration facts are keyed by
# MAME driver source file, while the machine and device data come from listxml.
DRIVER_INFO: dict[str, dict[str, Any]] = {
    "sony/psx.cpp": {"family": "PlayStation retail", "board_configs": ["SCPH retail console (NTSC/PAL)"], "ram": "2 MiB main RAM; 1 MiB VRAM (CXD8561Q configurations)", "peripherals": ["2 controller ports", "2 memory-card slots", "parallel I/O", "CD-ROM"], "support": "console core exists; this inventory concerns full machine integration"},
    "sony/zn.cpp": {"family": "Sony ZN / Capcom ZN / Tecmo TPS / Raizing ZN", "board_configs": ["ZN-1 COH-1000/1001/1002", "ZN-2 COH-3002", "Capcom/Tecmo/Raizing sound and I/O board variants"], "ram": "4 MiB base; some COH-1002/COH-3002 variants configure 8 MiB; GPU VRAM variants 1 or 2 MiB", "peripherals": ["board-specific sound CPU/ASIC", "security MCU/CAT702 on applicable boards", "game-specific controls and I/O"], "support": "native synthetic Capcom COH1000C/1002C/3002C and Taito FX1S COH1000T MIPS/Z80, banks, SIO/MCU, EEPROM, inputs, GPU and sound pass; authentic games and other daughterboards unverified"},
    "sony/taitogn.cpp": {"family": "Taito G-Net / FX-1-derived", "board_configs": ["Taito G-Net with ZN-derived COH-1002/COH-3002 CPU board", "Taito Zoom sound board"], "ram": "4 MiB base; exact per-config RAM/VRAM needs board-level validation", "peripherals": ["Taito Zoom sound (MN10200, TMS57002, ZSG-2)", "CompactFlash slot on applicable configurations", "game-specific I/O"], "support": "no arcade board support"},
    "konami/konamigv.cpp": {"family": "Konami System GV (Baby Phoenix)", "board_configs": ["System GV CPU/GPU board and game media variants"], "ram": "2 MiB main RAM; 1 MiB VRAM on CXD8514Q variant (other set-level GPU configs are listed per machine)", "peripherals": ["CD-ROM", "security and game-specific I/O"], "support": "all 15 source media profiles and local ROM/CHD preparation tested; native board execution pending"},
    "konami/konamigq.cpp": {"family": "Konami System GQ", "board_configs": ["System GQ"], "ram": "4 MiB main RAM; 2 MiB GPU memory", "peripherals": ["MC68000 and TMS57002 sound system", "game I/O"], "support": "complete source profile and local ROM/HDD preparation tested, including ROM transforms and default EEPROM; native board execution pending"},
    "konami/ksys573.cpp": {"family": "Konami System 573", "board_configs": ["System 573 CD-ROM", "System 573 flash media", "System 573 cassette/PC-card and I/O variants"], "ram": "4 MiB main RAM; video RAM is on the GPU board", "peripherals": ["ATA CD-ROM slots", "cassette slot", "PC-card slots", "security dongles and game-specific I/O"], "support": "native Mac Hyperbbc base flash P1/GL/NEON/SPU verified; analog/digital CD/cassette/sound variants pending"},
    "konami/twinkle.cpp": {"family": "Konami Twinkle", "board_configs": ["Twinkle DVD/game media board"], "ram": "4 MiB main RAM; GPU/VRAM size requires board-level validation", "peripherals": ["DVD/media and CD audio", "MC68000/RF5C400 audio board", "network/session I/O"], "support": "no arcade board support"},
    "namco/namcos10.cpp": {"family": "Namco System 10", "board_configs": ["System 10 main PCB and MEM PCB generations", "EXIO add-on I/O variants"], "ram": "4 MiB main RAM; 2 MiB GPU memory (2 × 256K×32 SGRAM)", "peripherals": ["MEM PCB", "optional EXIO", "CD-ROM on applicable sets", "game controls"], "support": "native synthetic MEM(M)/MEM(N) MIPS execution, bank fetch, NAND DMA, GPU and I/O pass; real game boot and add-on boards unverified"},
    "namco/namcos11.cpp": {"family": "Namco System 11", "board_configs": ["COH-100/COH-110 CPU PCB", "System 11 motherboard revisions", "ROM8/ROM8(64) daughterboards"], "ram": "4 MiB main RAM; 2 MiB GPU memory in MAME configs", "peripherals": ["C76 sound MCU", "C352 audio", "keycus/protection", "game-specific I/O"], "support": "COH100/COH110 native synthetic MIPS/C76 execution, eight executable banks, ADC inputs, C352 audio and EEPROM pass; real game boot unverified"},
    "namco/namcos12.cpp": {"family": "Namco System 12 / System 11 Plus", "board_configs": ["COH-700 / COH-716 CPU PCB", "System 12 motherboard revisions B/C", "M4F/M5F/M8F/M10X daughterboards", "CDXA and link/gun add-on PCBs"], "ram": "COH-700: 4 MiB main RAM; COH-716: 16 MiB; 2 MiB GPU memory", "peripherals": ["H8/3002 and game I/O", "JVS", "link/network", "gun I/O", "CDXA PCB on applicable sets"], "support": "Soul Calibur Asia C reaches attract and scripted coin/start/selection into a match; other profiles unverified; config registration is not evidence of working board support"},
}


def _text(parent: ET.Element, tag: str) -> str | None:
    node = parent.find(tag)
    return node.text.strip() if node is not None and node.text else None


def _machine_record(machine: ET.Element) -> dict[str, Any] | None:
    chips = [dict(ch.attrib) for ch in machine.findall("chip")]
    maincpu = next((c for c in chips if c.get("type") == "cpu" and c.get("tag") == "maincpu"), None)
    if not maincpu or not CPU_SIGNATURE.search(maincpu.get("name", "")):
        return None
    driver = machine.get("sourcefile", "")
    return {
        "set": machine.get("name"),
        "description": _text(machine, "description"),
        "driver": driver,
        "cloneof": machine.get("cloneof"),
        "romof": machine.get("romof"),
        "isbios": machine.get("isbios", "no") == "yes",
        "runnable": machine.get("runnable", "yes") == "yes",
        "mame_driver": dict(machine.find("driver").attrib) if machine.find("driver") is not None else {},
        "mame_features": sorted((dict(x.attrib) for x in machine.findall("feature")), key=lambda x: x.get("type", "")),
        "chips": sorted(chips, key=lambda x: (x.get("tag", ""), x.get("type", ""), x.get("name", ""))),
        "devices": sorted((dict(x.attrib) for x in machine.findall("device_ref")), key=lambda x: (x.get("tag", ""), x.get("name", ""))),
        "slots": sorted((dict(x.attrib) for x in machine.findall("slot")), key=lambda x: x.get("name", "")),
        "configurations": sorted((dict(x.attrib) for x in machine.findall("configuration")), key=lambda x: x.get("name", "")),
        "media_devices": sorted((dict(x.attrib) for x in machine.findall("device")), key=lambda x: (x.get("type", ""), x.get("tag", ""))),
    }


def inventory_from_xml(xml: str | bytes, source_commit: str = PINNED_MAME_COMMIT) -> dict[str, Any]:
    root = ET.fromstring(xml)
    records = [_machine_record(m) for m in root.findall("machine")]
    machines = sorted((x for x in records if x is not None), key=lambda x: x["set"] or "")
    grouped: dict[str, list[dict[str, Any]]] = {}
    for item in machines:
        grouped.setdefault(item["driver"], []).append(item)
    systems = []
    for driver, sets in sorted(grouped.items()):
        info = DRIVER_INFO.get(driver, {"family": "Unclassified PlayStation-derived board", "board_configs": [], "ram": "unknown", "peripherals": [], "support": "not assessed"})
        systems.append({"driver": driver, **info, "set_count": len(sets), "sets": sets})
    return {
        "schema_version": 1,
        "source": {"mame_version": root.get("build", "unknown"), "mame_commit": source_commit, "selection": "MAME listxml machine maincpu name matches Sony CXD85xx/CXD86xx family"},
        "completeness": {"machine_count": len(machines), "driver_count": len(systems), "counts_include_bios_roots": True, "cpu_signature": CPU_SIGNATURE.pattern, "exclusions": ["generic MIPS R3000/R3000A/R3041 without Sony CXD85xx/CXD86xx main CPU", "PlayStation 2 Emotion Engine systems"]},
        "systems": systems,
    }


def load_xml(listxml: Path | None, mame_bin: str) -> bytes:
    if listxml:
        return listxml.read_bytes()
    proc = subprocess.run([mame_bin, "-listxml"], check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return proc.stdout


def source_commit(source: Path) -> str:
    result = subprocess.run(["git", "-C", str(source), "rev-parse", "HEAD"], check=True, text=True, stdout=subprocess.PIPE)
    commit = result.stdout.strip()
    if commit != PINNED_MAME_COMMIT:
        raise ValueError(f"MAME source commit {commit} is not pinned commit {PINNED_MAME_COMMIT}")
    return commit


def main(argv: Iterable[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mame-source", type=Path, help="MAME checkout; must be at the pinned source commit")
    parser.add_argument("--mame-bin", default="mame", help="MAME executable used for -listxml (default: mame)")
    parser.add_argument("--listxml", type=Path, help="reuse a saved MAME -listxml XML file")
    parser.add_argument("--output", type=Path, default=Path("scripts/mame_psx_systems.json"))
    args = parser.parse_args(argv)
    try:
        commit = source_commit(args.mame_source) if args.mame_source else PINNED_MAME_COMMIT
        data = inventory_from_xml(load_xml(args.listxml, args.mame_bin), commit)
        encoded = json.dumps(data, indent=2, ensure_ascii=False, sort_keys=False) + "\n"
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    except (OSError, subprocess.CalledProcessError, ET.ParseError, ValueError) as exc:
        print(f"mame_psx_inventory: {exc}", file=sys.stderr)
        return 2
    print(f"wrote {data['completeness']['machine_count']} machines in {data['completeness']['driver_count']} drivers to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
