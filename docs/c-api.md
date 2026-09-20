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
- One session at a time per machine. `ewr_session_open` takes the same lock
  the CLI does and answers `EWR_ERR_ANOTHER_RUN` when another run holds the
  printer, because two runs sharing a printer take each other's replies.
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
| `ewr_plan` | no | `{"model", "target", "planned_writes": [{"address", "value"}]}` |
| `ewr_list_interfaces` | yes | `{"interfaces": [...]}`, indexes for `ewr_session_set_interface` |
| `ewr_detect_model` | yes | `{"device_id", "reported_model", "model"}` |
| `ewr_read_status` | yes | `{"model", "printer", "counters", "pads"}` |
| `ewr_dump` | yes | `{"model", "answered", "total", "values"}`; minutes, not seconds |
| `ewr_reset` | yes | `{"model", "phase", "writes", "verification", "before", "after", ...}` |

`ewr_reset` returns the same JSON whether it worked or not: `phase` says how
far it got, and the status code says what to tell the user.
