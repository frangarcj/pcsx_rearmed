#!/bin/sh
# Native PCSX ReArcade Mac harness, special System 12 cabinets.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
game=${1:-ptblank2}
case "$game" in
  ptblank2|ghlpanic|golgo13|g13knd|technodr|kartduel|aplarail|truckk|ujlnow) ;;
  *) printf '%s\n' 'Sets: ptblank2 ghlpanic golgo13 g13knd technodr kartduel aplarail truckk ujlnow' >&2; exit 2 ;;
esac
shift "$(( $# > 0 ? 1 : 0 ))"
cd "$root"
make -s -f Makefile.mac-harness BUILD=build/mac-harness-special GPU=vitagl-like DRC=1 SYSTEM12_TIMING=1 -j8
printf '%s\n' '5: coin · 1: start · arrows: controls/pedals · A/S/D/Space: buttons · F: gun sensor · Q/E: extra buttons · mouse: aim/fire · T/9: test/service · Escape: pause'
exec "$root/build/mac-harness-special/pcsx-harness" --system12 "$root/roms/$game.zip" --drc --interactive "$@"
