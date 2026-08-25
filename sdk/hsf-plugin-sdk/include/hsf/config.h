/* HSF Plugin SDK — configuration, provided BY the host TO the plugin.
 *
 * Scoped: every key a plugin asks for is resolved inside that plugin's own
 * configuration section. A plugin cannot read another plugin's settings, and
 * cannot reach the gateway's `auth` or `rest` sections, so a key name needs no
 * prefix and cannot collide.
 *
 * WHY THIS IS NOT A C++ STRUCT. The gateway currently declares one compiled
 * struct per protocol in ConfigManager (SerialConfig, ModbusConfig, ZkConfig,
 * …) with hand-written JSON mapping — see docs/architecture/current-state.md
 * §9. That is exactly what a plugin cannot have: Core would have to be
 * recompiled to learn a new protocol's settings, which is the thing the plugin
 * architecture exists to stop. A plugin's schema instead travels with it, as
 * the config.schema.json in its package, and Core stores and validates it
 * generically.
 *
 * Every getter takes a default and cannot fail. A missing or wrong-typed key
 * yields the default, because a driver that refuses to start over an absent
 * optional setting is worse than one that starts with a sane value and says so
 * in the log. Use `has` when absence is genuinely meaningful.
 */
#ifndef HSF_PLUGIN_SDK_CONFIG_H
#define HSF_PLUGIN_SDK_CONFIG_H

#include "hsf/error.h"

HSF_ABI_BEGIN

typedef struct HSFConfig HSFConfig; /* opaque, host-owned */

typedef struct {
  uint32_t struct_size;

  int32_t (*has)(const HSFConfig* self, HSFStr key);

  /* Borrowed for the duration of the call. Copy it if you keep it — the host
   * may be handing out a pointer into a JSON document it is free to reparse
   * when the operator saves the Configuration page. */
  HSFStr  (*get_str)(const HSFConfig* self, HSFStr key, HSFStr fallback);
  int64_t (*get_i64)(const HSFConfig* self, HSFStr key, int64_t fallback);
  double  (*get_f64)(const HSFConfig* self, HSFStr key, double fallback);
  int32_t (*get_bool)(const HSFConfig* self, HSFStr key, int32_t fallback);

  /* The whole section as a JSON object, for a driver with nested or repeated
   * configuration (a device table, a point map) that the flat getters cannot
   * express. Borrowed. */
  HSFStr  (*get_json)(const HSFConfig* self);

  /* Persist a value the plugin itself computed and wants back after a restart
   * — a learned device address, a discovered serial number. NOT for
   * operator-owned settings: those flow one way, from the Configuration page
   * into the plugin. Returns HSF_ERR_PERMISSION when the host has the section
   * read-only. */
  HSFStatus (*set_str)(HSFConfig* self, HSFStr key, HSFStr value);
} HSFConfigVTable;

typedef struct {
  const HSFConfigVTable* vt;
  HSFConfig*             self;
} HSFConfigRef;

/* NUL-terminated-key conveniences. hsf_cstr lives in error.h. */

static inline int64_t hsf_cfg_i64(HSFConfigRef c, const char* key, int64_t fallback) {
  if (!c.vt || !c.vt->get_i64) return fallback;
  return c.vt->get_i64(c.self, hsf_cstr(key), fallback);
}

static inline double hsf_cfg_f64(HSFConfigRef c, const char* key, double fallback) {
  if (!c.vt || !c.vt->get_f64) return fallback;
  return c.vt->get_f64(c.self, hsf_cstr(key), fallback);
}

static inline int hsf_cfg_bool(HSFConfigRef c, const char* key, int fallback) {
  if (!c.vt || !c.vt->get_bool) return fallback;
  return c.vt->get_bool(c.self, hsf_cstr(key), fallback ? 1 : 0) != 0;
}

static inline HSFStr hsf_cfg_str(HSFConfigRef c, const char* key, const char* fallback) {
  if (!c.vt || !c.vt->get_str) return hsf_cstr(fallback);
  return c.vt->get_str(c.self, hsf_cstr(key), hsf_cstr(fallback));
}

static inline int hsf_cfg_has(HSFConfigRef c, const char* key) {
  if (!c.vt || !c.vt->has) return 0;
  return c.vt->has(c.self, hsf_cstr(key)) != 0;
}

HSF_ABI_END

#endif /* HSF_PLUGIN_SDK_CONFIG_H */
