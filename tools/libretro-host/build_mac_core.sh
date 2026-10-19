#!/bin/sh
# Build the System 12 libretro core for macOS arm64 with the Vita's CPU/GPU
# choices (ari64, GPU NEON, no DRC compiler thread, profiler scopes) and the
# headless host. Makefile.libretro builds in its source tree, so this uses an
# isolated copy under OUT (default build/mac-libretro).
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/build/mac-libretro}
mkdir -p "$OUT/src"
rsync -a --delete --exclude build --exclude build-vita --exclude artifacts \
  --exclude roms --exclude covers --exclude .git --exclude '*.o' --exclude '*.d' --exclude '*.a' --exclude '*.vpk' \
  --exclude '*.elf' --exclude '*.velf' --exclude '*.dylib' "$ROOT/" "$OUT/src/"
# NDRC_THREAD=0 as on the Vita: the compiler thread makes ari64 runs
# nondeterministic, which would hide output differences.
make -C "$OUT/src" -f Makefile.libretro platform=osx ARCADE_SYSTEM12=1 \
  SYSTEM12_PROFILE=1 DYNAREC=ari64 BUILTIN_GPU=neon USE_ASYNC_GPU=0 \
  USE_ASYNC_SPU=1 USE_ASYNC_CDROM=0 USE_LIBRETRO_VFS=0 \
  HAVE_PHYSICAL_CDROM=0 NDRC_THREAD=0 -j"${PCSX_BUILD_JOBS:-2}"
xcrun clang -arch arm64 -O2 -g -std=gnu99 -Wall \
  -I"$ROOT/deps/libretro-common/include" \
  -o "$OUT/s12-libretro-host" "$ROOT/tools/libretro-host/s12_libretro_host.c"
echo "core: $OUT/src/pcsx_rearmed_libretro.dylib"
echo "host: $OUT/s12-libretro-host"
