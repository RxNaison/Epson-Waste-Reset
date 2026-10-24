#ifndef EWR_C_H
#define EWR_C_H

/* EWR's C ABI: what a program in another language links against.
 *
 * The shape of this API is chosen so that updating EWR does not mean
 * rewriting the program that calls it:
 *
 *   - Enumerators are appended, never renumbered, and never given a new
 *     meaning. An unknown value from a newer EWR must be treated as its
 *     category ("some failure"), not as a surprise.
 *   - Structured answers travel as UTF-8 JSON, in the shapes and under the
 *     promises of docs/json-output.md - the same ones `ewr --json` prints. A
 *     release may add keys; it will not remove, rename or retype one. That is
 *     why this header has no struct full of printer fields to keep in sync.
 *   - The one struct that does cross the boundary, ewr_event, starts with its
 *     own size. Read a field only after checking that `size` covers it.
 *   - Functions are added, not changed. ewr_abi_version() says which set is
 *     present.
 *
 * Strings EWR returns are UTF-8 and owned by EWR: release them with
 * ewr_string_free (out-parameters) unless documented as owned by the session.
 * Strings you pass in are borrowed for the duration of the call.
 *
 * A session is not thread-safe and callbacks arrive on the thread that made
 * the call. One session at a time per machine: EWR takes a lock, because two
 * runs sharing one printer take each other's replies.
 */

#include <stddef.h>

#if defined(_WIN32)
#  if defined(EWR_C_BUILD_SHARED)
#    define EWR_API __declspec(dllexport)
#  elif defined(EWR_C_USE_SHARED)
#    define EWR_API __declspec(dllimport)
#  else
#    define EWR_API
#  endif
#elif defined(EWR_C_BUILD_SHARED)
#  define EWR_API __attribute__((visibility("default")))
#else
#  define EWR_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Appended to, never renumbered. The names match the `error_code` strings of
 * the JSON contract, so one program can speak both. */
typedef enum ewr_status
{
    EWR_OK                   = 0,
    EWR_ERR_FAILED           = 1,  /* no more specific reason; read the message */
    EWR_ERR_INVALID_ARGUMENT = 2,
    EWR_ERR_ANOTHER_RUN      = 3,
    EWR_ERR_DEVICE_NOT_FOUND = 4,
    EWR_ERR_DATABASE         = 5,  /* database.json missing or unreadable */
    EWR_ERR_MODEL_UNKNOWN    = 6,
    EWR_ERR_NOT_SUPPORTED    = 7,  /* the model cannot do this */
    EWR_ERR_READ_FAILED      = 8,
    EWR_ERR_INCOMPLETE_DUMP  = 9,  /* some addresses went unread: not a backup */
    EWR_ERR_BLOCKED          = 10, /* a gate stopped it; nothing was written */
    EWR_ERR_WRITE_FAILED     = 11,
    EWR_ERR_WRITE_UNVERIFIED = 12, /* acknowledged, but the read-back disagreed */
    EWR_ERR_IO               = 13,
    EWR_ERR_MODEL_MISMATCH   = 14, /* the printer says it is something else */
    EWR_ERR_BAD_BACKUP       = 15, /* ABI 3: a backup or dump EWR cannot use */
    EWR_ERR_NOT_STARTED      = 16, /* ABI 3: sent, but the printer never set to work */
    EWR_ERR_PRINTER_ERROR    = 17  /* ABI 3: the printer reported an error while at it */
} ewr_status;

/* ABI 3. What ewr_head_clean asks for. */
typedef enum ewr_clean_group
{
    EWR_CLEAN_ALL   = 0,
    EWR_CLEAN_BLACK = 1,
    EWR_CLEAN_COLOR = 2
} ewr_clean_group;

typedef enum ewr_level
{
    EWR_LEVEL_TRACE   = 0,
    EWR_LEVEL_INFO    = 1,
    EWR_LEVEL_WARNING = 2,
    EWR_LEVEL_ERROR   = 3
} ewr_level;

typedef enum ewr_stage
{
    EWR_STAGE_GENERAL   = 0,
    EWR_STAGE_DATABASE  = 1,
    EWR_STAGE_UPDATE    = 2,
    EWR_STAGE_DETECT    = 3,
    EWR_STAGE_HANDSHAKE = 4,
    EWR_STAGE_READ      = 5,
    EWR_STAGE_WRITE     = 6,
    EWR_STAGE_VERIFY    = 7,
    EWR_STAGE_COMMIT    = 8
} ewr_stage;

/* One step of a run. `code` is the stable identifier to switch on; `message`
 * is human English that will change and be translated - never parse it. */
typedef struct ewr_event
{
    size_t      size;        /* bytes EWR filled in, starting at this field */
    const char* code;
    const char* message;
    int         level;       /* ewr_level */
    int         stage;       /* ewr_stage */
    int         index;       /* step number, or -1 */
    int         total;       /* steps in this sequence, or -1 */
    const char* fields_json; /* JSON object of string values, never NULL */
} ewr_event;

/* Pointers in `event` are valid only for the duration of the call. */
typedef void (*ewr_event_cb)(const ewr_event* event, void* user);

/* ABI 3. Handed the backup of every byte a write is about to overwrite, as
 * the JSON a backup file holds (docs/c-api.md), after the last gate and
 * before the first write. Return 1 once it is kept, 0 if it could not be:
 * that stops the write unless the blocker callback lets it go ahead. */
typedef int (*ewr_backup_cb)(const char* backup_json, void* user);

/* A question only the host can answer. Return 1 to go ahead, 0 to stop.
 * `json` describes what is being asked; see docs/c-api.md. */
typedef int (*ewr_decision_cb)(const char* json, void* user);

EWR_API const char* ewr_version(void);              /* "1.6.0" */
EWR_API int         ewr_abi_version(void);          /* this header's revision: 3 */
EWR_API int         ewr_json_contract_version(void);/* docs/json-output.md `v` */

/* "ok", "blocked", ... for logging. Never NULL, even for an unknown code. */
EWR_API const char* ewr_status_name(int status);

/* Frees a string EWR handed back through an out-parameter. NULL is fine. */
EWR_API void ewr_string_free(char* text);

typedef struct ewr_session ewr_session;

/* Loads the database and claims the printer for this process.
 * `database_path` may be NULL for "database.json" beside the caller's
 * working directory. Returns EWR_ERR_ANOTHER_RUN when another EWR already
 * holds the printer. */
EWR_API int  ewr_session_open(const char* database_path, ewr_session** out_session);

/* ABI 2. The same session for a printer on the network: `host` (an address
 * or a name) is reached over SNMP, UDP 161, for the session's whole life.
 * Status, plan and the waste pad reset work as over USB, with the same gates
 * and read-back; ewr_list_interfaces, ewr_dump and the ink reset need USB and
 * answer EWR_ERR_NOT_SUPPORTED, and the interface and soft-reset settings do
 * not apply. Takes the same lock as ewr_session_open. A NULL or empty `host`
 * is EWR_ERR_INVALID_ARGUMENT, never a USB session. A host that does not
 * resolve is EWR_ERR_DEVICE_NOT_FOUND, with the session still returned so
 * the reason can be read. */
EWR_API int  ewr_session_open_network(const char* database_path, const char* host,
                                      ewr_session** out_session);

EWR_API void ewr_session_close(ewr_session* session);

/* All optional; pass NULL to unset. Set before the call they apply to. */
EWR_API void ewr_session_set_event_callback(ewr_session* session, ewr_event_cb callback, void* user);
/* Without this callback a blocker stops the run: EWR does not assume consent. */
EWR_API void ewr_session_set_blocker_callback(ewr_session* session, ewr_decision_cb callback, void* user);
/* The unconditional ask-once gate before the first write. Without it, a reset
 * stops with EWR_ERR_BLOCKED - there is no implied yes. */
EWR_API void ewr_session_set_confirm_callback(ewr_session* session, ewr_decision_cb callback, void* user);

/* ABI 3. Without this callback a write keeps no backup of its own, and the
 * bytes come back only in the result's "backup" object - which a host that
 * crashes mid-write never sees. Set it to keep them before the write. */
EWR_API void ewr_session_set_backup_callback(ewr_session* session, ewr_backup_cb callback, void* user);

/* ABI 3. Whether the file at `path` is a database this EWR can load: the
 * same check the built-in updater runs on a download before it swaps it in.
 * For a host that downloads database.json itself. Needs no session.
 * {"models": n, "schema_version": n}; EWR_ERR_DATABASE when it is not one. */
EWR_API int ewr_database_check(const char* path, char** out_json);

/* ABI 3. Where EWR writes ewr_trace.log, for every session in the process.
 * NULL or "" is the current directory, the default. Set it before opening a
 * session, from a folder the process may write: an app's private storage. */
EWR_API void ewr_set_data_dir(const char* dir);

#if defined(__ANDROID__)
/* ABI 3, Android builds only. The printers the app opened through UsbManager
 * (UsbDeviceConnection.getFileDescriptor()), for every session in the
 * process. EWR never closes them. Call again whenever the set changes. */
EWR_API void ewr_android_set_usb_fds(const int* fds, size_t count);
#endif

/* 1-based interface from ewr_list_interfaces; 0 (the default) is automatic. */
EWR_API void ewr_session_set_interface(ewr_session* session, int candidate);
/* Diagnostic, Windows: reset the USB channel once, then wait for the printer
 * to re-initialize. Off by default. */
EWR_API void ewr_session_set_soft_reset(ewr_session* session, int enabled);

/* Writing one model's values into another printer is the mistake this whole
 * library is careful about, so ewr_reset asks the printer what it is and
 * refuses with EWR_ERR_MODEL_MISMATCH when the answer names a different
 * database entry. Allow it only when you have another way to be sure - the
 * printer is unlisted, say, or you are recovering a known clone. */
EWR_API void ewr_session_set_allow_model_mismatch(ewr_session* session, int allowed);

/* Why the last call failed, in English, owned by the session and valid until
 * the next call on it. Empty after a call that succeeded. */
EWR_API const char* ewr_session_last_error(ewr_session* session);

/* Every call below answers with JSON on success; free it with
 * ewr_string_free. The shapes are docs/json-output.md's `data` objects. */

/* {"interfaces": [...]} - what --interface <n> selects. */
EWR_API int ewr_list_interfaces(ewr_session* session, char** out_json);
/* {"device_id": ..., "model": ...} - what the printer says it is. */
EWR_API int ewr_detect_model(ewr_session* session, char** out_json);
/* ABI 2. {"printers": [{"address", "device_id", "reported_model",
 * "model_match"}]} - the Epson printers that answer a broadcast on this
 * machine's networks within about 2 s, as `ewr --list` shows them. Works on
 * any session, since the database names the models, and asks the printers
 * nothing but their device ID. An empty list is EWR_OK; EWR_ERR_IO means no
 * broadcast could be sent. Open the one the user picks with
 * ewr_session_open_network. */
EWR_API int ewr_discover_network(ewr_session* session, char** out_json);
/* {"models": [{"name": ..., "aliases": [...]}]} - no device needed. */
EWR_API int ewr_list_models(ewr_session* session, char** out_json);
/* {"model": ..., "target": ..., "planned_writes": [...]} - what a reset would
 * write, straight from the database. Touches no hardware. */
EWR_API int ewr_plan(ewr_session* session, const char* model, int ink, char** out_json);

/* {"model": ..., "printer": {...}, "counters": [...], "pads": [...]} */
EWR_API int ewr_read_status(ewr_session* session, const char* model, char** out_json);
/* {"model": ..., "answered": n, "total": n, "values": [...]}. Minutes, not
 * seconds. Writes no file: the bytes are yours to keep however you like.
 * Returns EWR_ERR_INCOMPLETE_DUMP - with the JSON still filled in - when the
 * printer left any byte unread, because a partial dump is not a backup. */
EWR_API int ewr_dump(ewr_session* session, const char* model, char** out_json);
/* Runs the reset lifecycle: preflight read, gates, writes, read-back.
 * `ink` selects the cartridge ink reset over the waste pads.
 * {"model": ..., "phase": ..., "writes": {...}, "verification": {...}, ...} */
EWR_API int ewr_reset(ewr_session* session, const char* model, int ink, char** out_json);

/* ABI 3. Writes back what `file_contents` - a backup file or an --dump file,
 * as text - holds of `model`'s own counter bytes: only the bytes a reset of
 * that model writes, and only those that differ. The same lifecycle as
 * ewr_reset, with the backup callback for what it overwrites. `model` may be
 * NULL for the one the file names; a file for another model is
 * EWR_ERR_BAD_BACKUP, as is one that cannot be read.
 * {"model", "target": "restore", "source", "planned_writes", "skipped",
 *  "missing", "unchanged", "phase", "writes", "verification", ...} */
EWR_API int ewr_restore(ewr_session* session, const char* model, const char* file_contents, char** out_json);

/* ABI 3. The expert restore: every byte of an --dump file that differs from
 * the printer, not only the counters - except the serial, head ID, USB ID and
 * network address, which are never written. It runs only on the unit the dump
 * came from (the serial both name, or else the dump's identity bytes all read
 * back unchanged); no callback can overrule that. `allow_unmapped` must be 1:
 * it says the caller knows these are bytes no reset writes. Builds without
 * EWR_RESTORE_ALL answer EWR_ERR_NOT_SUPPORTED. Same answer as ewr_restore. */
EWR_API int ewr_restore_all(ewr_session* session, const char* model, const char* dump_contents,
                            int allow_unmapped, char** out_json);

/* ABI 3. Read-only. {"model", "printer", "firmware", "serial", "serial_source",
 * "counters": [{"key", "value"}], "first_use", "fatal_errors": [{"kind",
 * "slot", "code"}]}. `model` may be NULL: then only the status and firmware. */
EWR_API int ewr_read_info(ewr_session* session, const char* model, char** out_json);

/* ABI 3. The printer's own maintenance, with the gates and confirm callback a
 * reset has, sent once and watched until the printer is done - minutes, not
 * seconds. No EEPROM byte is written, but a cleaning uses ink and fills the
 * waste pads. `model` may be NULL for a printer the database does not know:
 * the cleaning then goes as print data, which has no levels.
 *
 * `level`: "gentle", "normal", "medium", "strong" - what the model lists; NULL
 * is "normal". `power` non-zero is the ink charge instead of a cleaning, and
 * ignores `level`. `print_data` non-zero sends it as print data even where
 * the model has its own maintenance command.
 * {"model", "action", "level", "group", "phase", "route", "sent", "reply",
 *  "started", "seconds", "printer_before", "printer_after", "pads_before",
 *  "pads_after", "overrides"} */
EWR_API int ewr_head_clean(ewr_session* session, const char* model, const char* level, int group,
                           int power, int print_data, char** out_json);

/* ABI 3. Prints the nozzle check pattern: load plain paper. `variant` 1 asks
 * for the alternate pattern. Same answer as ewr_head_clean. */
EWR_API int ewr_nozzle_check(ewr_session* session, const char* model, int variant, char** out_json);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EWR_C_H */
