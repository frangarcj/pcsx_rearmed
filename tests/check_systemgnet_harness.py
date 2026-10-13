#!/usr/bin/env python3
"""Exercise the native G-Net PSX bus with an executed MIPS fixture."""
from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--harness", required=True, type=pathlib.Path)
    parser.add_argument("--drc", action="store_true",
                        help="run the guest fixture through the ARM64 dynamic recompiler")
    args = parser.parse_args()
    command = [str(args.harness), "--system-gnet-fixture", "--board-test"]
    if args.drc:
        command.append("--drc")
    run = subprocess.run(command,
                         text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         timeout=30)
    if run.returncode:
        raise RuntimeError(f"G-Net MIPS bus fixture failed ({run.returncode}):\n{run.stdout}")
    cpu = "drc" if args.drc else "interpreter"
    expected = (
        f"SystemGNet MIPS fixture: PASS cpu={cpu} const=2 bank1_flash=1234",
        "SystemGNet PCM fixture: PASS",
        "finite=1 monotonic=1 nonzero=1",
        "SystemGNet PCM sink disable: PASS",
        "SystemGNet SPU DMA fixture: PASS delayed_payload=1 ticks=24000 boundary_cycle=35431 madr_retained=1 read_wrap=1",
        "SystemGNet watchdog reset: PASS deferred=1 control=10",
        "SystemGNet set=coh3002t model=ZN2 ram=4194304 vram=2097152 gpu=type2",
        "reads=2 writes=1 unsupported=0 bank=1 fault=0 execution_verified=false",
        "SystemGNet media bios=coh3002t option=v1 card=NONE chd=none auth_boot=false",
        "SystemGNet source parent=none bios_model=COH-3002T machine=coh3002t input=taitogn init=empty_init rotation=ROT0 flags=MACHINE_IS_BIOS_ROOT",
    )
    missing = [token for token in expected if token not in run.stdout]
    if missing:
        raise RuntimeError(f"G-Net fixture report missing {missing}:\n{run.stdout}")
    print(run.stdout, end="")

    base_config = [
        "format=pcsx-aow-gnet-v1",
        "mame_set={set}",
        "mame_source_commit=deb2f54fe7300e7a192690318beb860159bfd52a",
        "board_model={model}",
        "cpu_device={cpu}",
        "cpu_clock_hz={cpu_hz}",
        "gpu_device={gpu}",
        "gpu_clock_hz=53693175",
        "main_ram_size=4194304",
        "vram_size=2097152",
        "game_execution_verified=false",
    ]

    def reject(directory: pathlib.Path, expected: str) -> None:
        run = subprocess.run([str(args.harness), "--system-gnet", str(directory)],
                             text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             timeout=10)
        if run.returncode == 0 or expected not in run.stdout:
            raise RuntimeError(f"G-Net loader rejection expected {expected!r}:\n{run.stdout}")

    with tempfile.TemporaryDirectory(prefix="pcsx-systemgnet-loader-") as temp:
        root = pathlib.Path(temp)
        invalid = root / "invalid"
        invalid.mkdir()
        (invalid / "board.ini").write_text("format=wrong\n", encoding="ascii")
        reject(invalid, "invalid or unsupported G-Net board.ini")

        wrong_set = root / "wrong-set"
        wrong_set.mkdir()
        text = "\n".join(line.format(set="unknown", model="ZN2", cpu="CXD8661R",
                                     cpu_hz="100000000", gpu="CXD8654Q")
                           for line in base_config) + "\n"
        (wrong_set / "board.ini").write_text(text, encoding="ascii")
        reject(wrong_set, "invalid or unsupported G-Net board.ini")

        wrong_option = root / "wrong-option"
        wrong_option.mkdir()
        text = "\n".join(line.format(set="coh3002t", model="ZN2", cpu="CXD8661R",
                                     cpu_hz="100000000", gpu="CXD8654Q")
                           for line in base_config)
        (wrong_option / "board.ini").write_text(
            text + "\nbios_option=unknown\n", encoding="ascii")
        reject(wrong_option, "invalid or unsupported G-Net board.ini")

        selected_game = root / "selected-game"
        selected_game.mkdir()
        text = "\n".join(line.format(set="chaoshea", model="ZN2", cpu="CXD8661R",
                                     cpu_hz="100000000", gpu="CXD8654Q")
                           for line in base_config)
        (selected_game / "board.ini").write_text(
            text + "\nchd_file=chaosheat.chd\noverlay_file=chaoshea.overlay\n",
            encoding="ascii")
        # Exact-sized synthetic BIOS region reaches the source profile hash
        # check; no ROM or CHD payload is provided or generated here.
        (selected_game / "maincpu.bin").write_bytes(bytes(0x80000))
        reject(selected_game, "SHA1 does not match pinned MAME region")

        wrong_chd = root / "wrong-chd"
        wrong_chd.mkdir()
        text = "\n".join(line.format(set="chaoshea", model="ZN2", cpu="CXD8661R",
                                     cpu_hz="100000000", gpu="CXD8654Q")
                           for line in base_config)
        (wrong_chd / "board.ini").write_text(
            text + "\nchd_file=wrong.chd\n", encoding="ascii")
        reject(wrong_chd, "invalid or unsupported G-Net board.ini")

        unsafe_overlay = root / "unsafe-overlay"
        unsafe_overlay.mkdir()
        (unsafe_overlay / "board.ini").write_text(
            text + "\nchd_file=chaosheat.chd\noverlay_file=../outside.overlay\n",
            encoding="ascii")
        reject(unsafe_overlay, "invalid or unsupported G-Net board.ini")

    chdman = shutil.which("chdman")
    if not chdman:
        raise RuntimeError("MAME chdman is required for the G-Net storage bus fixture")
    with tempfile.TemporaryDirectory(prefix="pcsx-systemgnet-storage-fixture-") as temp:
        root = pathlib.Path(temp)
        raw = root / "fixture.raw"
        source = root / "fixture-source.chd"
        chd = root / "fixture.chd"
        raw.write_bytes(bytes((n * 13 + 7) & 0xff for n in range(20 * 512)))
        subprocess.run([chdman, "createhd", "-i", str(raw), "-o", str(source),
                        "-chs", "2,2,5", "-ss", "512", "-hs", "1536",
                        "-c", "none"], check=True, stdout=subprocess.DEVNULL)
        for tag, payload in (("CIS ", b"\x01\x02\xfe\xff"),
                             ("KEY ", bytes((0x10, 0x21, 0x32, 0x43, 0x54)) )):
            metadata = root / f"{tag.strip()}.meta"
            metadata.write_bytes(payload)
            subprocess.run([chdman, "addmeta", "-i", str(source), "-t", tag,
                            "-vf", str(metadata)], check=True,
                           stdout=subprocess.DEVNULL)
        subprocess.run([chdman, "copy", "-i", str(source), "-o", str(chd),
                        "-c", "zlib"], check=True, stdout=subprocess.DEVNULL)
        command = [str(args.harness), "--system-gnet-storage-fixture", str(chd)]
        if args.drc:
            command.append("--drc")
        storage_run = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, timeout=60)
        expected_storage = (
            "SystemGNet storage MIPS fixture: PASS",
            "type1_unlock=1 ata_identify_first=848a ata_pio_sector_first=1407",
            "SystemGNet storage reset fixture: PASS type1_relocked=1",
            "card=TYPE1_TEST chd=synthetic auth_boot=false fixture=true",
            "main_clock_input_hz=100000000",
        )
        missing_storage = [item for item in expected_storage
                           if item not in storage_run.stdout]
        if storage_run.returncode or missing_storage:
            raise RuntimeError(
                f"G-Net storage MIPS fixture failed; missing {missing_storage}:\n"
                f"{storage_run.stdout}")
        print(storage_run.stdout, end="")
        reopen_run = subprocess.run(
            command + ["--storage-reopen-check"], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
        expected_reopen = (
            "SystemGNet storage MIPS fixture: PASS",
            "ata_pio_sector_first=5aa5",
            "overlay_write=1 reopen=1",
            "card=TYPE1_TEST chd=synthetic auth_boot=false fixture=true",
            "main_clock_input_hz=100000000",
        )
        missing_reopen = [item for item in expected_reopen
                          if item not in reopen_run.stdout]
        if reopen_run.returncode or missing_reopen:
            raise RuntimeError(
                f"G-Net ATA overlay reopen failed; missing {missing_reopen}:\n"
                f"{reopen_run.stdout}")
        print(reopen_run.stdout, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
