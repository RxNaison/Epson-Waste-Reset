# Contributing to EWR

## Add or fix a printer - the short version

Edit `database.json`, open a pull request. That is the whole workflow.

The build script (`scripts/build_db.py`) treats the committed `database.json`
as the curated source of truth: **whatever the file says, wins.** Every
upstream source (reinkpy, ez-reset, reink, gutenprint) is merged *around*
your values - the build only fills in fields you left out and appends models
nobody has committed yet. A rebuild can never overwrite a hand edit.

### Fixing a value

Change it in place. Your value is kept even when every upstream source
disagrees; the weekly rebuild prints any deliberate divergence in its
summary, so nothing gets flattened silently.

### Testing an edited database locally

Run EWR with `--no-update` while iterating: nothing is downloaded and the
staged swap on exit is skipped, so `database.json` stays exactly as you wrote
it. Without the flag, a run that reaches the update server replaces a
hand-edited `database.json` with the upstream one on exit - so keep passing
`--no-update` until your pull request is merged.

### Adding a model

Add a new entry with at least:

    "rkey", "wkey", "rlen", "wlen", "addresses", "reset"

`rlen`/`wlen` are the **address field** length in bytes - 1 for the old
R220-class models, 2 for modern ones. `wkey` may need trailing `\u0000`
padding on older models. If an upstream source knows the model too, the
build fills in anything you left out: counter maps, service limits,
detection aliases, the commit step, the firmware-recovery channel.

### Un-pinning a value

To deliberately re-pull a field from upstream, delete the field (or the whole
model) and rebuild - it refills from the sources.

### What to put in the PR

Say what you changed and how you verified it. The gold standard is a run
against the real printer showing:

- write packets acknowledged (`:42:OK;`),
- a read-back of the reset addresses returning the expected values,
- the printer leaving the error state.

Paste the console output into the PR - it stays archived there, attached to
the exact diff it proves.

### Rebuilding locally (optional)

```sh
python scripts/build_db.py build   # fetches every upstream, merges around your edits
```

`build` fetches all upstream sources over the network and merges them around
the committed file - your values always win. Skipping it is fine too: the
weekly automation runs the same command and commits anything new it finds.

## Hardware nothing supports yet

If no database entry exists anywhere, capture a working reset with Wireshark
and open a **New Printer Model Submission** issue. The replay engine is how
new hardware gets its first reset before any database entry exists, and a
capture is ground truth whenever an entry is disputed.

## Code contributions

- C++17, CMake. Every change lands with tests and the suite stays green
  (CI builds Windows + Linux on every PR).
- The core library never writes to a console - diagnostics leave through
  event sinks (`include/ewr/log.h`). The CLI is just one sink over the same
  API a GUI would use.
- Anything that changes what gets **written** to a printer needs verification
  on at least one real unit before it ships in a release.
