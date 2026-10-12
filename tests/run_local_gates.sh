#!/bin/sh
# Gates that need the ROMs or MAME, so they cannot run in GitHub Actions: H8
# unit tests, MAME comparisons and Soul Calibur replays, on macOS arm64.
#
#   tests/run_local_gates.sh [OUT [BASELINE]]
#
# OUT (default build/local-gates/<commit>) keeps every log and capture. With
# BASELINE, an earlier OUT, the replays must equal it byte for byte (frame,
# audio and System 12 report); run it once before a change to get one.
# ROM (roms/soulclbr.zip) and MAME (/opt/homebrew/bin/mame) can be overridden.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2
OUT=${1:-build/local-gates/$(git rev-parse --short HEAD)}
BASELINE=${2:-}
ROM=${ROM:-roms/soulclbr.zip}
MAME=${MAME:-/opt/homebrew/bin/mame}
HARNESS=build/mac-harness-s12src/pcsx-harness
DRC_HARNESS=build/mac-harness-drc-timing/pcsx-harness
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
[ -f "$ROM" ] || { echo "missing $ROM"; exit 2; }
[ -x "$MAME" ] || { echo "missing MAME at $MAME"; exit 2; }
failed=0

step() {
    name=$1
    shift
    printf '%-24s' "$name"
    if "$@" > "$OUT/$name.log" 2>&1; then
        echo ok
    else
        echo "FAIL  $OUT/$name.log"
        failed=1
    fi
}

h8_units() {
    make -f tests/Makefile.h8 test irq-test deadline-ubsan-test spin-skip-ubsan-test \
        cycle-clock-ubsan-test sci-sync-timer-ubsan-test sci-sync-timer-fixture &&
    make -f tests/Makefile.h8-adc test ubsan &&
    make -f tests/Makefile.h8_timer16 test ubsan-test &&
    make -f tests/Makefile.h8-sci test ubsan
}

# The board's source timing is how the Vita runs System 12; the RTC gate
# needs it with the recompiler as well.
harnesses() {
    make -f Makefile.mac-harness GPU=vitagl-like SYSTEM12_TIMING=1 \
        BUILD=build/mac-harness-s12src -j8 &&
    make -f Makefile.mac-harness DRC=1 SYSTEM12_TIMING=1 \
        BUILD=build/mac-harness-drc-timing -j8
}

sync_timers() {
    python3 tests/check_mame_h8_sync_timers.py --mame "$MAME" \
        --output "$OUT/mame-sync-timers.json" --log "$OUT/mame-sync-timers-mame.log" &&
    python3 tests/check_h8_sync_timers.py --mame "$OUT/mame-sync-timers.json" \
        --native build/h8-tests/h8-sync-timer-fixture --output "$OUT/native-sync-timers.json"
}

rtc() {
    python3 tests/check_mame_system12_rtc.py --mame "$MAME" \
        --output "$OUT/mame-rtc.json" --log "$OUT/mame-rtc-mame.log" &&
    python3 tests/check_system12_rtc.py --harness "$DRC_HARNESS" \
        --mame "$OUT/mame-rtc.json" --output "$OUT/native-rtc.json"
}

# --frontend-loop runs the board like the Vita; without it the board syncs
# every 256 cycles, which exercises the H8's partial states.
replay() {
    name=$1
    shift
    "$HARNESS" --system12 "$ROM" "$@" --output "$OUT/$name.ppm" --audio "$OUT/$name.wav"
}

report() {
    grep '^System12:' "$1" | grep -v 'perf\|us/frame\|loop calls'
}

same_replay() {
    name=$1
    cmp "$OUT/$name.ppm" "$BASELINE/$name.ppm" &&
    cmp "$OUT/$name.wav" "$BASELINE/$name.wav" &&
    report "$OUT/replay-$name.log" > "$OUT/$name.report" &&
    report "$BASELINE/replay-$name.log" | diff - "$OUT/$name.report"
}

step h8-units h8_units
step harnesses harnesses
step mame-h8 python3 tests/check_mame_h8.py --mame "$MAME" --rom "$ROM" --output "$OUT/mame-h8"
step mame-peripherals python3 tests/check_mame_h8_peripherals.py --mame "$MAME" --include-sci \
    --output "$OUT/mame-peripherals.json"
step mame-sync-timers sync_timers
step mame-rtc rtc
step replay-attract replay attract --frontend-loop --frames 12000
step replay-sliced replay sliced --frames 3000
if [ -n "$BASELINE" ]; then
    step same-attract same_replay attract
    step same-sliced same_replay sliced
fi
grep -h 'H8 perf' "$OUT/replay-attract.log" 2>/dev/null
exit $failed
