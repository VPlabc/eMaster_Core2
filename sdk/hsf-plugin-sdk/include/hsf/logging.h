/* HSF Plugin SDK — logging, provided BY the host TO the plugin.
 *
 * A plugin must not open its own log file. The gateway already has two
 * deliberately separate channels — a runtime/debug log (console, rotating file,
 * in-memory ring the Log Viewer tails) and a durable structured business record
 * — and a plugin writing beside them would be invisible to both. Both are
 * reached here.
 *
 * Levels mirror the host's own so nothing is lost in translation.
 */
#ifndef HSF_PLUGIN_SDK_LOGGING_H
#define HSF_PLUGIN_SDK_LOGGING_H

#include "hsf/error.h"

HSF_ABI_BEGIN

typedef enum {
  HSF_LOG_TRACE = 0,
  HSF_LOG_DEBUG = 1,
  HSF_LOG_INFO  = 2,
  HSF_LOG_WARN  = 3,
  HSF_LOG_ERROR = 4
} HSFLogLevel;

typedef struct HSFLogger HSFLogger; /* opaque, host-owned */

typedef struct {
  uint32_t struct_size;

  /* Runtime/debug channel. `msg` is borrowed for the duration of the call.
   * The plugin's id is prepended by the host, so do not repeat it in msg. */
  void (*write)(HSFLogger* self, HSFLogLevel level, HSFStr msg);

  /* Cheap level test. Call this before building an expensive message —
   * formatting a hex dump that the host then discards is pure cost on a
   * 256 MB device. */
  int32_t (*enabled)(const HSFLogger* self, HSFLogLevel level);

  /* Durable structured business record: a declared type plus a JSON object of
   * fields, validated by the host against that type's declaration. Returns
   * HSF_ERR_NOT_FOUND when the type was never declared, rather than inventing
   * it — an undeclared type is a bug, not a new schema. */
  HSFStatus (*record)(HSFLogger* self, HSFStr type, HSFStr json_fields);
} HSFLoggerVTable;

/* Fat pointer. Every host service crosses the ABI in this shape: the vtable
 * says what can be done, `self` says to whom. Passing them as one struct means
 * a plugin cannot accidentally pair a vtable with the wrong instance. */
typedef struct {
  const HSFLoggerVTable* vt;
  HSFLogger*             self;
} HSFLoggerRef;

/* Convenience wrappers. NUL-terminated C string in, nothing to remember about
 * lengths. Guarded so a plugin holding a zeroed context cannot crash on a log
 * call — which is exactly when it most wants to log. */
static inline void hsf_log(HSFLoggerRef log, HSFLogLevel level, const char* msg) {
  if (!log.vt || !log.vt->write || !msg) return;
  {
    HSFStr s;
    size_t n = 0;
    while (msg[n] != '\0') ++n;
    s.ptr = msg;
    s.len = n;
    log.vt->write(log.self, level, s);
  }
}

static inline int hsf_log_enabled(HSFLoggerRef log, HSFLogLevel level) {
  if (!log.vt || !log.vt->enabled) return 0;
  return log.vt->enabled(log.self, level) != 0;
}

#define HSF_LOG_I(logref, msg) hsf_log((logref), HSF_LOG_INFO, (msg))
#define HSF_LOG_W(logref, msg) hsf_log((logref), HSF_LOG_WARN, (msg))
#define HSF_LOG_E(logref, msg) hsf_log((logref), HSF_LOG_ERROR, (msg))

HSF_ABI_END

#endif /* HSF_PLUGIN_SDK_LOGGING_H */
