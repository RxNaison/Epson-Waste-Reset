# `--json`: machine-readable output

`ewr --json` turns stdout into a stream of JSON objects, one per line (JSON
Lines), for callers driving EWR from another language. Nothing else is written
to stdout in this mode: human text goes to stderr, the hardware trace goes to
`ewr_trace.log` as always.

```bash
ewr --status --model L3150 --json --no-update
```

```json
{"v":1,"type":"hello","seq":0,"t":0,"ewr":"1.3.1","platform":"windows","command":"status","model":"L3150","flags":{...}}
{"v":1,"type":"event","seq":1,"t":31,"code":"db.loaded","level":"info","stage":"database","message":"...","index":null,"total":null,"fields":{}}
{"v":1,"type":"result","seq":9,"t":9412,"command":"status","ok":true,"exit":0,"error_code":null,"error":null,"data":{...}}
```

## What you may rely on

These are the promises. Everything else is an implementation detail.

1. **Exactly one `hello` first and one `result` last**, on every run, whatever
   goes wrong - including a printer that never answered and a command line that
   was rejected. If the stream ends without a `result`, EWR was killed.
2. **A consumer that reads only `hello`, `result` and the exit code never needs
   updating.** Events are progress detail; the outcome is the `result` line.
3. **Within a contract version `v`**, no key is removed, renamed or changes
   type, and no `code` or `error_code` is reused for a different meaning.
4. **Documented keys are always present**, with `null` where a value is unknown.
   Treat an absent key the same as `null` anyway - see rule 5.
5. **New things may appear in any release**: new event `code`s, new `fields`
   keys, new `data` keys, new `error_code` values, new line `type`s. A consumer
   must ignore what it does not recognize rather than fail.
6. **`message` is human English.** It is free to change and will eventually be
   translated. Never parse it, never match on it. The same goes for anything on
   stderr.
7. **Numbers are numbers.** EEPROM addresses and values are integers, not
   `"0x2B"` strings. Only `fields` is string-to-string, because it carries
   whatever an event has to say.
8. **If the contract ever has to break**, `v` goes up and the previous version
   stays available through `--json-version <n>` for at least two releases.

## Envelope

Every line carries these:

| Key | Type | Meaning |
| --- | --- | --- |
| `v` | int | Contract version. `1` today. |
| `type` | string | `hello`, `event` or `result`. Ignore any other value. |
| `seq` | int | 0-based, one up per line. A gap means a line was lost. |
| `t` | int | Milliseconds since the run started. |

## `hello`

| Key | Type | Meaning |
| --- | --- | --- |
| `ewr` | string | EWR version, e.g. `"1.3.1"`. |
| `platform` | string | `windows`, `linux` or `macos`. |
| `command` | string | `status`, `dump`, `list`, `dry-run`, `reset` or `find-addresses`. |
| `model` | string/null | What `--model` named, before matching. `null` when not given. |
| `flags` | object | The switches that change behavior: `no_update`, `yes`, `force_yes`, `cartridge`, `usb_soft_reset`, `interface` (int, 0 = automatic). |

## `event`

| Key | Type | Meaning |
| --- | --- | --- |
| `code` | string | Stable identifier, e.g. `exec.write_verified`. Switch on this. |
| `level` | string | `info`, `warning` or `error`. Trace detail stays in `ewr_trace.log`. |
| `stage` | string | `general`, `database`, `update`, `detect`, `handshake`, `read`, `write`, `verify`, `commit`. |
| `message` | string | Human English. Do not parse. |
| `index` | int/null | Step number within a sequence of `total`, when there is one. |
| `total` | int/null | Length of that sequence. |
| `fields` | object | String to string. Keys are as stable as `code`. |

Codes are namespaced by layer (`db.`, `usb.`, `exec.`, `d4.`, `end4.`,
`update.`). A release can add codes; it will not repurpose one.

## `result`

| Key | Type | Meaning |
| --- | --- | --- |
| `command` | string | Same value as in `hello`. |
| `ok` | bool | True only when the command did what it was asked to. |
| `exit` | int | The process exit code: `0` done, `1` stopped or failed, `2` bad command line. |
| `error_code` | string/null | Stable reason when `ok` is false. |
| `error` | string/null | Human detail. Do not parse. |
| `data` | object | Command payload, below. Present (possibly empty) on every result. |

### `error_code` values

| Value | Meaning |
| --- | --- |
| `bad_usage` | The command line was rejected. Exit 2. |
| `another_run` | Another EWR run holds the printer. |
| `device_not_found` | No Epson interface answered. |
| `interface_not_found` | `--interface <n>` names an interface that is not present. |
| `database` | `database.json` is missing, empty or unreadable. |
| `model_required` | The command needs a model and none was given or detected. |
| `model_unknown` | `--model` matched nothing usable. |
| `model_ambiguous` | `--model` matched several models. |
| `model_mismatch` | `--model` names something other than the printer that answered. |
| `not_supported` | The model cannot do this (a Replay dump has no read key, no ink map for `--cartridge`). |
| `read_failed` | The printer did not answer the read. |
| `incomplete_dump` | Some addresses went unread; the file is not a backup. |
| `blocked` | A gate stopped the run: a printer error, a database conflict, or a confirmation that was not given. Nothing was written. |
| `write_failed` | The write session failed. Some writes may have been applied. |
| `write_unverified` | Writes were acknowledged but the read-back did not confirm them. |
| `io_error` | A file could not be written. |
| `failed` | The run stopped for a reason with no more specific code. Read `error`. A release may replace it with a narrower code, so treat it as "something went wrong", never as a particular thing. |

Every way a run can stop names one of these. `failed` is the last resort, not
the common case: if you meet it where the table promises something narrower,
that is a bug worth reporting - a caller is promised it can act on `result`
alone, and a code it cannot branch on breaks that promise.

### `data` by command

**status** and **dry-run**

```json
{
  "model": "L3150",
  "detected_model": "L3150",
  "printer": {"state": "ERROR", "state_code": 0, "error": "INK OUT", "error_code": 5,
              "truncated": false, "serial": null,
              "maintenance_box": null, "maintenance_box_status": null,
              "inks": [{"color": "Black", "code": 0, "level": 43, "status": "OK"}]},
  "counters": [{"address": 12, "value": 0}],
  "pads": [{"name": "Main Pad Counter", "kind": "main", "used": 0, "max": 46750, "percent": 0}],
  "planned_writes": [{"address": 12, "value": 0}]
}
```

`kind` is how you tell one pad from another without reading English: `"main"`,
`"platen"`, or `null` when neither the counter nor its pad group says which. It
describes *that counter*, not the group it is filed under - 489 models keep a
platen counter inside a group marked `main`, so the counter's own description
decides and the group is only the fallback. Act on the kind, never on `name` -
a platen counter taken for the main one is a real bug that has happened. `used` and `max` are there so you can judge the number yourself
instead of trusting a rounded `percent`, which is `null` when the model has no
service limit on record.

`null` always means "not reported", never zero: an ink the printer says nothing
about has `"level": null` with its own `status` text, while an empty cartridge
has `"level": 0`. Same for `maintenance_box`, which carries its condition in
`maintenance_box_status` because a box can report one without a level.

`detected_model` is what the printer said it was, when the caller ran detection
- `null` means nobody asked, not that nothing matched. `ewr --status` fills it;
the C API leaves it null and offers `ewr_detect_model` instead.

`planned_writes` is `null` for `status` and the list a reset would write for
`dry-run`. A counter that went unread has `value: null`.

`pads` holds the pads that could be read whole; `pads_total` is how many the
model has. They differ when a pad group's bytes did not all come back, so
`pads: [], pads_total: 2` means "could not read them", while `pads_total: 0`
means "this model has none" - an empty `pads` alone cannot tell you which.

**A `dry-run` that could not read the printer fails.** It still reports the
plan, because the plan comes from the database and is worth seeing, but it
returns `error_code: "read_failed"` and exit 1 with `printer: null` - a plan
under `ok: true` would read as "this is what your printer needs", and nothing
was read from any printer.

`status` and `dry-run` fill `data` whether the read worked or not, so a caller
that got nothing still learns which model it asked about and how many pads that
model has: `printer: null`, `counters: []`, `pads: []`, and `pads_total` from
the database.

A Replay model carries no read key, so its `status` has empty `counters` and
`pads`, and its `dry-run` reports `planned_writes: null` plus `replay_packets`
and `replay_writes`: a captured dump is opaque bytes, and their count is all
that can be said about it before it is sent.

**dump**

```json
{"model": "R220", "file": "ewr_dump_R220_1789854673.txt", "answered": 256, "total": 256,
 "values": [{"address": 0, "value": 0}]}
```

**list**

```json
{"interfaces": [{"index": 1, "class": "USBPRINT", "interface_number": -1, "path": "\\\\?\\usb#...",
                 "device_id": "MFG:EPSON;...", "model_match": "R220"}]}
```

`index` is what `--interface <n>` takes.

**reset**

```json
{"model": "R220", "target": "waste", "phase": "done",
 "writes": {"verified": 5, "total": 5}, "alternate_key_used": false, "committed": true,
 "verification": {"ran": true, "mismatches": 0, "unread": 0},
 "before": [{"address": 12, "value": 0}], "after": [{"address": 12, "value": 0}]}
```

`target` is `waste` or `ink`. `phase` is `not_started`, `aborted`,
`device_not_found`, `write_failed` or `done`.

**find-addresses**

```json
{"model": "R220", "file": "R220-found-addresses.json", "passes": 3, "short_passes": 0,
 "trends": [{"addresses": [12, 13], "pair": true, "values": [0, 40, 91],
             "deltas": [40, 51], "direction": "rising"}]}
```

## Answering gates without a keyboard

`--json` never prompts: there is no one to ask. A run that would need an answer
stops with `error_code: "blocked"` and writes nothing. Give the answers on the
command line instead - `--yes` for the reset confirmation, `--force-yes` to
overrule a printer error or a database conflict as well, `--model` when the
menu would otherwise open. `--force-yes` is the one that can write to a printer
in a state EWR would rather not touch; see the README before reaching for it.
