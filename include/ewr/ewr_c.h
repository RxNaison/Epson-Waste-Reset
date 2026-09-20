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
    EWR_ERR_MODEL_MISMATCH   = 14  /* the printer says it is something else */
} ewr_status;

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

/* A question only the host can answer. Return 1 to go ahead, 0 to stop.
 * `json` describes what is being asked; see docs/c-api.md. */
typedef int (*ewr_decision_cb)(const char* json, void* user);

EWR_API const char* ewr_version(void);              /* "1.3.1" */
EWR_API int         ewr_abi_version(void);          /* this header's revision */
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
EWR_API void ewr_session_close(ewr_session* session);

/* All optional; pass NULL to unset. Set before the call they apply to. */
EWR_API void ewr_session_set_event_callback(ewr_session* session, ewr_event_cb callback, void* user);
/* Without this callback a blocker stops the run: EWR does not assume consent. */
EWR_API void ewr_session_set_blocker_callback(ewr_session* session, ewr_decision_cb callback, void* user);
/* The unconditional ask-once gate before the first write. Without it, a reset
 * stops with EWR_ERR_BLOCKED - there is no implied yes. */
EWR_API void ewr_session_set_confirm_callback(ewr_session* session, ewr_decision_cb callback, void* user);

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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* EWR_C_H */
