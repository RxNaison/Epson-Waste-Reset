# Contributing to EWR

## Add or fix a printer - the short version

Edit `database.json`, open a pull request. That is the whole workflow.

The build script (`scripts/build_db.py`) treats the committed `database.json`
as the curated source of truth: **whatever the file says, wins.** Every
upstream source (reinkpy, ez-reset, reink, gutenprint) is merged *around*
your values - the build only fills in fields you left out and appends models
nobody has committed yet. A rebuild can never overwrite a hand edit.

### The file layout

`database.json` is a schema 4 envelope. Printers live under `"models"`, and
models that share an identical body point at a shared block in `"specs"`
instead of repeating it:

```json
{
    "schema_version": 4,
    "specs": {
        "L3110-family": { "rkey": 1943, "wkey": "Nbsjcbzb", "pad_groups": [ ... ] }
    },
    "models": {
        "L3110": { "spec": "L3110-family" },
        "L3150": { "spec": "L3110-family" },
        "R220":  { "rkey": 2000, "wkey": "Arkanoid", "pad_groups": [ ... ] }
    }
}
```

A key written on the model itself overrides the shared block, so to change one
printer out of a family you add the field to its entry rather than editing the
spec group everybody else rides. Spec groups are regenerated on every rebuild -
never hand-name one; just write the fields you want and the build folds the
duplicates back together.

### Fixing a value

Change it in place. Your value is kept even when every upstream source
disagrees; the weekly rebuild prints any deliberate divergence in its
summary, so nothing gets flattened silently.

### Testing an edited database locally

Run EWR with `--no-update` while iterating: nothing is downloaded, so
`database.json` stays exactly as you wrote it. Without the flag, a run that
reaches the update server replaces a hand-edited `database.json` with the
upstream one at startup, before your edit is ever used - so keep passing
`--no-update` until your pull request is merged.

### Adding a model

Add an entry under `"models"` with at least:

    "rkey", "wkey", "rlen", "wlen", "addresses", "reset"

`addresses` is the flat list of EEPROM bytes to write and `reset` the value for
each, positionally paired. That minimal shape is enough - the build turns it
into the `pad_groups` the current format uses, and EWR reads it either way.

`rlen`/`wlen` are the **address field** length in bytes - 1 for the old
R220-class models, 2 for modern ones. `wkey` may need trailing `\u0000`
padding on older models. If an upstream source knows the model too, the
build fills in anything you left out: counter maps, service limits,
detection aliases, the commit step.

### Maintenance and identity keys (1.6)

The build fills these in from ez-reset where it knows the model. None of them is
ever part of a reset, and EWR before 1.6 ignores them:

| Key | What it is | Used by |
| --- | --- | --- |
| `cleaning` | `{"clean": {"normal": [2], ...}, "black": {...}, "color": {...}, "charge": {"normal": [7]}}` - the payload of the keyed maintenance command (action `0x84`) per level. Only a one-byte payload is ever sent, and `0xFF` never is. | `--head-clean`, `--power-clean` |
| `serial` | The EEPROM addresses of the serial number, one ASCII character each. | `--info`, the restore's unit check |
| `identity` | `{"head_id": [...], "usb_id": [...], "net_id": [...]}` - bytes that make the unit this unit. `--restore-all` never writes them. | `--restore-all` |
| `report` | `[{"key": "total_print_page_counter", "addresses": [...]}]` - the printer's own counters, least significant byte first. | `--info` |
| `first_use` | The addresses of the date the printer was first used. | `--info` |
| `fatal` | `[{"kind": "printer", "slot": 1, "addresses": [...]}]` - where the last fatal error codes are kept. | `--info` |

A malformed one of these costs the maintenance, never the model: the entry
still loads and resets. Fill a gap the same way as any other value - by hand,
with a source.

Models read in a mode that takes no key (the SX110 family's JIG mode,
`"access": "jig"`) live under a top-level `"keyless_models"`, not `"models"`:
EWR before 1.6 reads only `"models"` and would load such an entry as a keyed
model with key 0. Add one there, and never put `"access": "jig"` under
`"models"`.

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

### Code layout

`include/ewr/` is what a host programs against (the CLI, the C ABI, a GUI).
Everything else sits next to its `.cpp` under `src/protocol/`,
`src/transport/` and `src/update/`, and public headers never include those.

- `ewr::` holds types. A free function lives in a namespace named for what it
  works on - `ctrl`, `d4`, `usb`, `json`, `backup`, `restore`, `gates`, ... -
  except `ToString(enum)` overloads, which ADL finds next to their enum.
- Classes hold state or stand at a seam (`ITransport`, `IDeviceGateway`,
  `usb::Backend`); pure transforms are free functions. No static-only classes
  and no static mutable state: every printer must stay fakeable in a test.
- A name does not repeat its namespace: `json::ResetData`, not
  `json::JsonResetData`.

`scripts/check_layout.py` checks the first two rules and the include rule; it
runs with the tests (`ctest`) wherever Python is installed.
