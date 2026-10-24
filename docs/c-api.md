# The C API: embedding EWR

`include/ewr/ewr_c.h` is EWR's C ABI, for a program that wants the reset
*inside* it - a GUI, a service, a language binding. If you only need to run
EWR and read what happened, spawn `ewr --json` instead: no linking, no build,
and the same JSON ([docs/json-output.md](json-output.md)).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

That produces the shared library beside the CLI: `ewrc.dll` on Windows,
`libewrc.so` on Linux, `libewrc.dylib` on macOS. Link `ewr_core` instead to
get the same functions statically; pass `-DEWR_BUILD_C_SHARED=OFF` to skip the
shared library. Callers that include the header against the shared library
should define `EWR_C_USE_SHARED` first.

## What will not change under you

The point of this API is that a newer EWR does not mean a rewrite.

- **Enumerators are appended, never renumbered**, and never given a new
  meaning. Treat an unfamiliar value as its category, not as a bug.
- **Structured answers are JSON**, in the shapes of the JSON contract, with
  the same promises: keys may be added, never removed, renamed or retyped.
  That is why no struct full of printer fields crosses the boundary - the one
  that does, `ewr_event`, starts with its own `size`.
- **Functions are added, not changed.** `ewr_abi_version()` tells you which
  set you have.

## Rules

- Strings are UTF-8. A `char**` out-parameter is yours to release with
  `ewr_string_free`; everything else EWR returns is owned by the session and
  valid until the next call on it.
- A session is not thread-safe, and callbacks arrive on the thread that made
  the call. Do not call back into the session from inside a callback.
- One session at a time per machine. `ewr_session_open` and
  `ewr_session_open_network` take the same lock the CLI does, over USB or the
  network alike, and answer `EWR_ERR_ANOTHER_RUN` when another run holds it,
  because two runs sharing a printer take each other's replies.
- **Nothing is assumed on your behalf.** Without a confirm callback a reset
  stops with `EWR_ERR_BLOCKED`; without a blocker callback, so does a printer
  that reports an error. Silence is never consent to write.
- **A wrong-model write is refused.** `ewr_reset` asks the printer what it is
  and returns `EWR_ERR_MODEL_MISMATCH` when the answer names a different
  database entry, because one model's addresses in another printer's EEPROM is
  the mistake worth preventing. `ewr_session_set_allow_model_mismatch` turns
  the check off for an unlisted printer you have identified another way. A
  printer that reports nothing recognizable cannot contradict you, so it does
  not block the run.
- `ewr_dump` fills its JSON *and* returns `EWR_ERR_INCOMPLETE_DUMP` when any
  byte went unread. The bytes are yours either way; the code says they are not
  a backup.

## A printer on the network (ABI 2)

`ewr_session_open_network` is the library side of the CLI's `--ip`: the same
session, reaching the printer over SNMP (UDP 161) instead of USB for its whole
life. Check `ewr_abi_version() >= 2` before calling it.

```c
ewr_session* session = NULL;
int rc = ewr_session_open_network(NULL, "192.168.1.100", &session);
```

To find the address, `ewr_discover_network` on any session (a USB one, say)
asks this machine's networks which Epson printers are there and answers
`{"printers": [{"address", "device_id", "reported_model", "model_match"}]}`
within about 2 s, the same list `ewr --list` prints. Let the user pick: a
network can hold someone else's Epson. An empty list is `EWR_OK`;
`EWR_ERR_IO` means this machine would not send the broadcast. A USB session
holds the run lock, so close it before opening the network one.

- `ewr_detect_model`, `ewr_read_status` and the waste pad `ewr_reset` work as
  over USB, with the same gates, confirmation and read-back; the database
  calls need no printer either way.
- `ewr_list_interfaces`, `ewr_dump` and the ink reset answer
  `EWR_ERR_NOT_SUPPORTED`: none has been tried over the network.
  `ewr_session_set_interface` and `ewr_session_set_soft_reset` do not apply.
- A NULL or empty `host` is `EWR_ERR_INVALID_ARGUMENT`, never a USB session in
  its place.
- A host name that does not resolve is `EWR_ERR_DEVICE_NOT_FOUND` from the
  open itself. The session is still returned, so `ewr_session_last_error` can
  say why, and the database calls work on it.
- A printer that never answers is `EWR_ERR_DEVICE_NOT_FOUND` from the first
  call that needs it, and `ewr_reset` stops there, before the confirm callback
  is asked anything. The last error tells silence apart from a send this
  machine refused: a VPN, a firewall, or an operating system keeping the
  program off the local network.
- Many recent Epson models, and older ones after a firmware update, lock the
  EEPROM over the network but not over USB. A printer that answers but refuses
  every EEPROM read raises `snmp.eeprom_refused`, and `ewr_reset` then returns
  `EWR_ERR_NOT_SUPPORTED` (`phase` `not_supported`) before the confirm
  callback is asked anything. Over USB the same printer usually works.
- The network transport's own events are namespaced `snmp.`.

## Backups, restore, maintenance and Android (ABI 3)

Check `ewr_abi_version() >= 3` before calling any of these.

**Backups.** Every write - a reset, a restore - first builds a backup of the
bytes it is about to overwrite, as the preflight read them, and returns it in
the result's `"backup"` object. To keep it *before* the write, where a crash
mid-write cannot lose it, set a backup callback:

```c
static int keep_backup(const char* backup_json, void* user)
{
    /* Write backup_json to a file you can find again; 1 once it is kept. */
    return save_somewhere(backup_json) ? 1 : 0;
}

ewr_session_set_backup_callback(session, keep_backup, NULL);
```

`backup_json` is a whole backup file (format in json-output.md). A callback
that returns 0 stops the write unless the blocker callback lets it go on
(`error: "BACKUP NOT SAVED"`), and a backup missing bytes the preflight could
not read asks the same way (`"BACKUP INCOMPLETE"`). Without the callback
nothing is gated on a backup, as in ABI 2.

**Restore.** `ewr_restore(session, model, file_contents, &json)` takes the
text of a backup file or an `--dump` file and writes back what it holds of
`model`'s own counter bytes - only those, only the ones that differ - through
the reset's lifecycle: preflight, gates, the confirm callback (`target:
"restore"`), a backup of what it overwrites, writes, read-back. `model` may be
NULL for the one the file names. A file EWR cannot use, one for another model,
or one with none of the model's bytes is `EWR_ERR_BAD_BACKUP`.

`ewr_restore_all(session, model, dump_contents, 1, &json)` writes every byte of
a dump that differs, except the serial, head-ID, USB-ID and network bytes, and
only on the unit the dump came from. It is in builds made with
`-DEWR_RESTORE_ALL=ON` only; elsewhere it answers `EWR_ERR_NOT_SUPPORTED`.

**Maintenance.** `ewr_head_clean(session, model, level, group, power,
print_data, &json)` and `ewr_nozzle_check(session, model, variant, &json)` run
the printer's own maintenance with the gates and confirm callback a reset has
(`target`: `head-clean`, `power-clean` or `nozzle-check`, plus `route`), send
it once, and watch the printer's status until it is done - minutes, not
seconds. `EWR_ERR_NOT_STARTED` means the printer took the command but never set
to work; `EWR_ERR_PRINTER_ERROR` that it reported an error while at it. `group`
is `EWR_CLEAN_ALL`, `EWR_CLEAN_BLACK` or `EWR_CLEAN_COLOR`. `model` may be NULL
for a printer the database does not know: the cleaning then goes as print data.

**Info.** `ewr_read_info(session, model, &json)` reads firmware, serial, the
printer's own counters and its last fatal error codes. Read-only.

**A host that updates the database itself.** `ewr_database_check(path, &json)`
runs the check the built-in updater runs on a download - EWR loads the file -
and needs no session. Swap the file in only on `EWR_OK`.

**Where EWR writes.** `ewr_set_data_dir(dir)` moves `ewr_trace.log` out of
the current directory, for every session in the process. An app sets it to
its private storage before the first session.

**Android.** The library builds with the NDK (the app in `android/` does it).
An app may not enumerate USB devices itself, so it opens the printer through
`UsbManager` and hands EWR the file descriptors:
`ewr_android_set_usb_fds(fds, count)`, again whenever the set changes. EWR
wraps them, claims the interfaces on them, and never closes them. Set
`TMPDIR` to the app's cache directory for the one-run-at-a-time lock.

## A whole run, in C

```c
#include <ewr/ewr_c.h>
#include <stdio.h>

static void on_event(const ewr_event* event, void* user)
{
    (void)user;
    if (event->total > 0)
        printf("[%d/%d] %s\n", event->index, event->total, event->code);
}

static int on_blocker(const char* json, void* user)
{
    (void)user;
    printf("the printer objects: %s\n", json);
    return 0;  /* 1 would push past it, as --force-yes does */
}

static int on_confirm(const char* json, void* user)
{
    (void)user;
    printf("about to write: %s\n", json);
    return 1;
}

int main(void)
{
    ewr_session* session = NULL;
    int rc = ewr_session_open(NULL, &session);
    if (rc != EWR_OK)
    {
        fprintf(stderr, "%s: %s\n", ewr_status_name(rc), ewr_session_last_error(session));
        ewr_session_close(session);
        return 1;
    }

    ewr_session_set_event_callback(session, on_event, NULL);
    ewr_session_set_blocker_callback(session, on_blocker, NULL);
    ewr_session_set_confirm_callback(session, on_confirm, NULL);

    char* json = NULL;
    rc = ewr_reset(session, "L3150", 0, &json);
    printf("%s %s\n", ewr_status_name(rc), json ? json : "");
    ewr_string_free(json);

    ewr_session_close(session);
    return rc == EWR_OK ? 0 : 1;
}
```

## The same from Python, with no binding to build

```python
import ctypes, json

class Event(ctypes.Structure):
    _fields_ = [("size", ctypes.c_size_t), ("code", ctypes.c_char_p), ("message", ctypes.c_char_p),
                ("level", ctypes.c_int), ("stage", ctypes.c_int), ("index", ctypes.c_int),
                ("total", ctypes.c_int), ("fields_json", ctypes.c_char_p)]

EVENT_CB = ctypes.CFUNCTYPE(None, ctypes.POINTER(Event), ctypes.c_void_p)
DECIDE_CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_char_p, ctypes.c_void_p)

ewr = ctypes.CDLL("./build/Release/ewrc.dll")   # libewrc.so / libewrc.dylib
ewr.ewr_status_name.restype = ctypes.c_char_p
ewr.ewr_status_name.argtypes = [ctypes.c_int]
ewr.ewr_session_last_error.restype = ctypes.c_char_p
ewr.ewr_session_last_error.argtypes = [ctypes.c_void_p]

@EVENT_CB
def on_event(event, user):
    e = event.contents
    if e.total > 0:
        print(f"[{e.index}/{e.total}] {e.code.decode()}")

@DECIDE_CB
def on_confirm(payload, user):
    state = json.loads(payload)
    print("about to reset", state["model"], state["target"])
    return 1

session = ctypes.c_void_p()
rc = ewr.ewr_session_open(b"database.json", ctypes.byref(session))
if rc != 0:
    raise SystemExit(ewr.ewr_status_name(rc).decode())

ewr.ewr_session_set_event_callback(session, on_event, None)
ewr.ewr_session_set_confirm_callback(session, on_confirm, None)

out = ctypes.c_char_p()
rc = ewr.ewr_reset(session, b"L3150", 0, ctypes.byref(out))
result = json.loads(out.value)
ewr.ewr_string_free(out)

if rc == 0:
    print("verified", result["writes"]["verified"], "of", result["writes"]["total"])
else:
    print(ewr.ewr_status_name(rc).decode(), ewr.ewr_session_last_error(session).decode())

ewr.ewr_session_close(session)
```

Keep a reference to the callback objects for as long as the session lives:
ctypes frees a thunk that nothing holds, and EWR will call it.

## What each call answers

| Call | Needs a printer | Answers |
| --- | --- | --- |
| `ewr_list_models` | no | `{"models": [{"name", "aliases", "resettable", "has_ink_reset"}]}` |
| `ewr_plan` | no | `{"model", "target", "planned_writes": [{"address", "value"}], "reset_covers": [{"kind", "name", "readable"}]}`; `reset_covers` is `[]` for an ink reset |
| `ewr_list_interfaces` | yes | `{"interfaces": [{"index", "class", "interface_number", "path", "device_id", "model_match"}]}`, the same entries as `--list`; `index` is what `ewr_session_set_interface` takes |
| `ewr_detect_model` | yes | `{"device_id", "reported_model", "model"}` |
| `ewr_discover_network` | on the network | `{"printers": [{"address", "device_id", "reported_model", "model_match"}]}`; `address` is what `ewr_session_open_network` takes |
| `ewr_read_status` | yes | `{"model", "detected_model", "printer", "counters", "pads", "pads_total", "reset_covers"}`; `detected_model` stays null here, ask `ewr_detect_model` |
| `ewr_dump` | yes | `{"model", "answered", "total", "values"}`; minutes, not seconds |
| `ewr_reset` | yes | `{"model", "phase", "writes", "verification", "before", "after", "overrides", "backup", ...}`; `overrides` names each gate your callbacks, or `ewr_session_set_allow_model_mismatch`, let it past |
| `ewr_restore`, `ewr_restore_all` | yes | `ewr_reset`'s keys with `target: "restore"`, plus `"source", "planned_writes", "skipped", "missing", "unchanged"` |
| `ewr_head_clean`, `ewr_nozzle_check` | yes | `{"model", "action", "level", "group", "phase", "route", "sent", "reply", "started", "seconds", "printer_before", "printer_after", "pads_before", "pads_after", "overrides"}` |
| `ewr_read_info` | yes | `{"model", "printer", "firmware", "serial", "serial_source", "counters", "first_use", "fatal_errors"}` |
| `ewr_database_check` | no | `{"models", "schema_version"}` |

`ewr_reset` returns the same JSON whether it worked or not: `phase` says how
far it got, and the status code says what to tell the user.

## What the callbacks are asked

| Callback | `json` |
| --- | --- |
| blocker | `{"error", "error_code", "explanation"}`: `error` names the objection (`INK OUT`, `PRINTER BUSY`, `DATABASE CONFLICT`, ...), `error_code` is the printer's error code and `null` when the objection is not a printer error, `explanation` is English for a dialog |
| confirm | what `ewr_read_status` answers for the preflight, plus `"target"`: `waste`, `ink`, `restore`, `head-clean`, `power-clean` or `nozzle-check`; maintenance adds `"route"` |
| backup | a whole backup file: `{"format": "ewr-backup", "version", "created", "model", "target", "printer", "address_length", "bytes", "complete"}` |

The same rules hold as for answers: keys may be added, and `null` is never a
stand-in for zero.
