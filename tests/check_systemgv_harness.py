#!/usr/bin/env python3
"""Exercise the shared Konami GV adapter with a real synthetic CHD target."""
from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--harness", required=True, type=pathlib.Path)
    parser.add_argument("--chdman", default="chdman")
    parser.add_argument("--drc", action="store_true",
                         help="run the synthetic guest through the DRC backend")
    args = parser.parse_args()
    chdman = shutil.which(args.chdman)
    if not chdman:
        parser.error("MAME chdman is required for the real CHD fixture")
    with tempfile.TemporaryDirectory(prefix="pcsx-systemgv-") as temp:
        root = pathlib.Path(temp)
        data = root / "data.bin"
        with data.open("wb") as stream:
            for lba in range(8):
                stream.write(bytes(((lba * 31 + i * 7 + 3) & 0xff)
                                   for i in range(2048)))
        cue = root / "disc.cue"
        cue.write_text('FILE "data.bin" BINARY\n'
                       '  TRACK 01 MODE1/2048\n'
                       '    INDEX 01 00:00:00\n', encoding="ascii")
        chd = root / "disc.chd"
        subprocess.run([chdman, "createcd", "-i", str(cue), "-o", str(chd),
                        "-c", "none"],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        command = [str(args.harness), "--system-gv-fixture", str(chd), "--board-test"]
        if args.drc:
            command.append("--drc")
        run = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=45)
        if run.returncode:
            raise RuntimeError(f"GV fixture failed ({run.returncode}):\n{run.stdout}")
        expected = ("SystemGV selftest: PASS", "SystemGV MIPS fixture: PASS",
                    "SystemGV DMA write fixture: PASS CHCR=01000201 block_size=512 remaining=0",
                    "DMA5_pending=1048576 CHCR_completed=1 IRQ10=1",
                    "IRQ10=1 DRQ=0 DMA=3 requests remaining=1048576",
                    "CD_DMA_words=12",
                    "faults=00000000/00000000")
        missing = [token for token in expected if token not in run.stdout]
        if missing:
            raise RuntimeError(f"GV fixture report missing {missing}:\n{run.stdout}")
        expected_cpu = "drc" if args.drc else "interpreter"
        if f"SystemGV MIPS fixture: PASS cpu={expected_cpu}" not in run.stdout:
            raise RuntimeError(f"GV fixture did not execute the requested CPU backend:\n{run.stdout}")
        print(run.stdout, end="")
        bad_profile = root / "bad-profile"
        bad_profile.mkdir()
        (bad_profile / "board.ini").write_text("format=pcsx-aow-system-gv-v1\n",
                                                encoding="ascii")
        rejected = subprocess.run([str(args.harness), "--system-gv", str(bad_profile)],
                                  text=True, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, timeout=10)
        if rejected.returncode == 0 or "malformed or incomplete Konami GV board.ini" not in rejected.stdout:
            raise RuntimeError(f"normal loader did not reject incomplete profile: {rejected.stdout}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
