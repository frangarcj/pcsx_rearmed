# Workspace storage

Read `docs/build-storage.md` before builds or emulator validation. Disk exhaustion
has already interrupted this work; do not accumulate another set of raw captures.

- Before every build or validation batch, run
  `python3 scripts/check_validation_storage.py --reserve-mib N`, with an estimate
  of all new output. Do not run the batch if this check fails or bypass its floor.
  Chain the guard and batch with `&&` in one shell command, or explicitly check
  the returned exit code before calling another tool. Printing its result alone
  is not a gate.
- Keep at least 4 GiB free after the estimated output. Limit a batch's new raw
  captures to 256 MiB. Reuse the existing Mac build directories.
- After comparing a batch, retain its commands, diagnostics, hashes, compact CSV
  observations and useful PNGs; remove generated WAV/RAM/VRAM/PPM intermediates.
  The storage guard blocks the next batch while any such files remain in
  `artifacts/`. Finish cleanup before starting another run.
  Do not rerun old tests just to restore captures removed for disk cleanup.
- Preserve source, original ROMs/CHDs, installed NVRAM, runnable applications and
  the historical source/executable archive. Never remove these as disposable
  captures. Do not make another media copy when an existing input suffices.
  Prepared CHDs must reference the verified original rather than duplicate it.
