# Build storage and historical evidence

On 2026-10-08, `build/` occupied 35,855,237,120 allocated bytes (33.39 GiB).
Most of it was accumulated isolated Vita3K diagnostic builds: copied app
bundles, firmware/runtime trees, source snapshots and repeated compilation
archives. The retired groups alone contained 12.64 GiB of `host-fix-obj`
compilation caches. No compiler, harness or emulator was running during cleanup.

43 historical Vita build directories were retired after preserving every file
except those compilation caches in a content-addressed gzip archive. Identical
files are stored once. Each newly created archive object was decompressed and
verified against its original SHA-256 before removal. Logs, screenshots,
packages, modified source and historical executables are retained. Recovery
was tested on an evidence/package directory, a Vita3K executable and a symlink.

| Storage after cleanup | Allocated bytes | GiB |
| --- | ---: | ---: |
| Remaining `build/` | 3,424,620,544 | 3.19 |
| Compressed historical archive and manifest | 1,709,703,168 | 1.59 |
| Net reclaimed, accounting for the archive | 30,720,913,408 | 28.61 |

Current Mac builds and device tests remain in place. Vita dependencies remain
in `build/vita-deps`, the latest H8 IRQ guest build in
`build/vita-system12-h8-irq-20261005`, and the recent runnable host in
`build/vita-validation-mspace-stats-fix2`. Retained NEON/GL harnesses and the
Vita3K executable have unchanged hashes. ROM archives and tracked source were
not part of the retirement.

The local, ignored archive is
`artifacts/validation/build-cleanup-20261008/`: `plan.json`, `manifest.json`,
`summary.json`, `restore-check.json`, `retained-hashes.json`, and `objects/`.
The manifest maps the original relative file paths to their compressed content
hashes, modes and timestamps. Recover an old build or individual file with:

```sh
python3 artifacts/validation/build-cleanup-20261008/restore.py build/vita-validation-preserve-target
python3 artifacts/validation/build-cleanup-20261008/restore.py build/vita-validation/host-fix-src --destination /tmp/pcsx-history
```

Restoration refuses to overwrite existing files or traverse symlink parents.
The excluded `host-fix-obj` caches are rebuilt from the retained source/patches
and original build commands when needed. This local archive is not a remote
backup; preserve it when moving or removing the workspace.

On 2026-10-15, another cleanup removed 5,429 generated audio/memory/raw-frame
captures and rebuildable object files (10.43 GiB allocated), leaving 11.46 GiB
free. Original media, installed NVRAM, applications and the historical archive
were retained. Commands, diagnostics, CSV observations, PNGs and capture hashes
remain; removed raw files are historical evidence and must not be reported as
freshly reverified. The ignored cleanup manifest is
`artifacts/disk-cleanup-20261015.jsonl`.

For future work:

- Before every build or validation batch, run
  `python3 scripts/check_validation_storage.py --reserve-mib N` on the output
  volume, reserving the estimated total new output. The check refuses batches
  that would leave less than 10 GiB free; do not bypass that floor.
- The guard also refuses a new batch while WAV/RAM/VRAM/PPM captures remain
  in `artifacts/`. Preserve compact observations and hashes, then remove those
  intermediates before proceeding. It does not automatically delete anything.
- Chain a preflight and its batch with `&&`, or inspect the tool's exit code
  before launching it. A printed rejection does not stop a later independent
  tool call. An agent orchestration error launched two GL object compilations
  after a failed preflight; the objects were then removed and the required
  gating rule made explicit in `AGENTS.md`.
- Mac validation raw captures have a 256 MiB budget per batch. Compare and hash
  the outputs, preserve commands/diagnostics and compact observations, then
  discard WAV/RAM/VRAM/PPM intermediates. Keep useful PNGs and installed NVRAM.
  Previously pruned captures need not be regenerated to rerun an evidence-only
  verifier; its old PASS remains a historical result, not a fresh verification.
- Reuse the active Mac build directory for incremental builds rather than
  retaining a new complete build for every experiment.
- Keep one current Vita3K runtime and a separate control only when a comparison
  needs it. Keep diagnostic outputs in `artifacts/validation` and retire old
  runtimes after preserving their exact source, executable and evidence.
- `rebuild_vita3k_fixed.py` retains the signed app, patched source and hash
  manifest, but discards intermediate objects after a successful build.
  `--keep-objects` retains them for link/build diagnosis; failed builds retain
  their intermediates automatically.
- Avoid copying ROMs, previous `build/` trees or accumulated `artifacts/` into
  source snapshots. Store exact inputs once and reference them by hashes.
- A further cleanup on 2026-10-15 removed 29 raw captures (315.45 MiB allocated)
  and replaced 13 byte-identical CHD copies with relative links to preserved
  originals after SHA-256 comparison. This recovered about 2.7 GiB of actual
  free space in total (APFS copies can share blocks). The operation manifest is
  `artifacts/disk-cleanup-20261015-followup.jsonl`; source media, installed NVRAM,
  executables and the historical archive remain available.
- System GV preparation can use `--link-chd` to reference an audited standalone
  original disc. Full CHD/source checks still run; the prepared manifest records
  the external path, which must remain available. Repreparing a linked profile
  does not copy its old disc into the staging directory. Temporary extracted
  archive members and links into the output package are rejected.
- All `build/` outputs are ignored by Git; commits contain source and reproducible
  tests, with local validation evidence kept separately.

Another same-day cleanup during pad-read diagnostics removed 31.97 MiB of
rebuildable objects and 25.00 MiB of remaining generated RAM/VRAM `.bin` files.
Seventeen byte-identical media copies (169 MiB) were converted to independent
APFS copy-on-write clones after SHA-256 checks, preserving each destination's
metadata. These are separate inodes: future writes do not modify their peers.
The clone operation reclaimed 168.85 MiB of actual blocks. Every original path
and byte remains available. The operation manifest is
`artifacts/disk-cleanup-20261015-pad-reads.jsonl`. New raw captures were pruned;
no emulator/build process was left running. Shared-volume free space changed
during the work, and no further emulation batch was launched near the floor.
