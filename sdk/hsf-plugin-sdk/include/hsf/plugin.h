/* HSF Plugin SDK — the plugin entry points. Include this and you have the SDK.
 *
 * A plugin is a shared object exporting exactly THREE C symbols:
 *
 *     hsf_plugin_abi_version()   what it was built against; called first, and
 *                                safe to call before anything else exists
 *     hsf_plugin_get_info()      static description; no instance needed
 *     hsf_plugin_create()        make an instance
 *     hsf_plugin_destroy()       unmake it
 *
 * (Four, counting destroy — but destroy is the pair of create, not a separate
 * capability.)
 *
 * The version symbol is separate from get_info deliberately. If it lived inside
 * HSFPluginInfo, the host would have to read a struct whose layout is exactly
 * what it does not yet know is compatible. A bare uint32 return can be trusted
 * across any version, so the compatibility check never depends on the thing it
 * is checking.
 *
 * HSF_PLUGIN_DEFINE at the bottom writes all of this for you.
 */
#ifndef HSF_PLUGIN_SDK_PLUGIN_H
#define HSF_PLUGIN_SDK_PLUGIN_H

#include "hsf/config.h"
#include "hsf/driver.h"
#include "hsf/event.h"
#include "hsf/logging.h"

HSF_ABI_BEGIN

typedef enum {
  HSF_PLUGIN_KIND_DRIVER    = 1,  /* a protocol driver: Modbus, ZK, BACnet */
  HSF_PLUGIN_KIND_TRANSPORT = 2,  /* supplies a transport the host lacks */
  HSF_PLUGIN_KIND_SERVICE   = 3   /* neither: a bridge, an exporter */
} HSFPluginKind;

/* Declared capabilities, checked against the manifest's "permissions" array.
 * The manifest is the operator-visible contract and this is the runtime one;
 * the host cross-checks them at load and refuses a plugin that asks in code
 * for something its manifest does not declare (plan §14). */
#define HSF_PERM_SERIAL   0x0001u
#define HSF_PERM_NETWORK  0x0002u
#define HSF_PERM_FILESYS  0x0004u
#define HSF_PERM_GPIO     0x0008u
#define HSF_PERM_EXEC     0x0010u  /* spawn a process — expect scrutiny */
#define HSF_PERM_LUA      0x0020u  /* register Lua bindings */
#define HSF_PERM_DB       0x0040u  /* its own SQLite tables */

typedef struct {
  uint32_t struct_size;

  /* Reverse-DNS, stable forever: it keys configuration, install state and
   * rollback. Renaming it orphans the operator's settings. */
  HSFStr id;          /* "hsf.driver.modbus" */
  HSFStr name;        /* "HSF Modbus Driver" */
  HSFStr version;     /* semver, the PLUGIN's own */
  HSFStr description;
  HSFStr author;

  HSFPluginKind kind;
  uint32_t      permissions;   /* HSF_PERM_* */

  /* What it was built for, from HSF_PLATFORM_TRIPLE. The host compares this to
   * its own and refuses a mismatch with a message naming both, instead of
   * letting the dynamic loader fail with something unreadable. */
  HSFStr platform;
} HSFPluginInfo;

/* Everything the host hands a plugin at creation. This struct only ever grows
 * by appending, guarded by struct_size — a plugin built against 1.0 keeps
 * working on a 1.1 host that added a field it has never heard of. */
typedef struct {
  uint32_t struct_size;

  /* The host's API version. A plugin MAY inspect it to use a slot added in a
   * later minor, but must not require one. */
  uint32_t host_api_version;

  /* Host services. */
  HSFLoggerRef           log;
  HSFConfigRef           config;       /* scoped to this plugin's section */
  HSFEventBusRef         events;
  HSFTransportFactoryRef transports;

  /* Writable directory owned by this plugin, for caches and its own database.
   * Borrowed; copy it. A plugin must not write anywhere else — the host may
   * mount the rest read-only. */
  HSFStr data_dir;

  /* This plugin's instance name, when the operator has configured the same
   * plugin more than once ("modbus-floor1", "modbus-floor2"). Empty for a
   * singleton. Use it in log messages or two instances are indistinguishable
   * in the log. */
  HSFStr instance_id;
} HSFPluginContext;

typedef struct HSFPlugin HSFPlugin; /* opaque, plugin-owned */

/* The instance vtable, returned by create alongside the instance pointer. */
typedef struct {
  uint32_t struct_size;

  /* Driver access. NULL for a non-driver plugin; the host checks `kind` first
   * and does not ask. */
  HSFStatus (*get_driver)(HSFPlugin* self, HSFDriverRef* out, HSFDriverInfo* info);

  /* Register Lua bindings. Called with the host's lua_State* as void* — the SDK
   * refuses to include lua.h, because forcing every plugin author to match the
   * host's exact Lua version and build flags to compile a header is the kind of
   * coupling this SDK exists to remove. A plugin that wants Lua includes its
   * own lua.h and casts.
   *
   * Register into package.preload, not as a global: that is what the gateway's
   * existing zk_controller module does, and it keeps the global namespace from
   * becoming a free-for-all as plugins accumulate.
   *
   * Requires HSF_PERM_LUA. NULL when the plugin has no Lua surface. */
  HSFStatus (*register_lua)(HSFPlugin* self, void* lua_state);

  /* Reload after the operator saved new settings, without a restart. Return
   * HSF_ERR_NOT_SUPPORTED and the host will stop and start instead — an honest
   * refusal, and correct for a driver that cannot re-key a live connection. */
  HSFStatus (*reconfigure)(HSFPlugin* self, HSFConfigRef config);

  /* Free-form diagnostics for the Test Tool and the plugin's status page.
   * `out_json` is borrowed and valid until the next call. */
  HSFStatus (*diagnostics)(HSFPlugin* self, HSFStr* out_json);
} HSFPluginVTable;

/* ---------------------------------------------------------------------------
 * Exported symbols. Names are ABI; do not decorate them.
 * ------------------------------------------------------------------------- */

/* Called FIRST, before get_info, before create. Must be safe to call on a
 * plugin the host is about to reject. */
HSF_PLUGIN_EXPORT uint32_t hsf_plugin_abi_version(void);

/* Static, process-lifetime storage. Called with no instance in existence, so it
 * must not depend on one. */
HSF_PLUGIN_EXPORT const HSFPluginInfo* hsf_plugin_get_info(void);

/* On success writes both out-params and returns HSF_OK. On failure returns
 * negative and must leave nothing allocated — the host will not call destroy
 * for a create that failed. `ctx` is borrowed for the duration of the call;
 * copy anything you keep. */
HSF_PLUGIN_EXPORT HSFStatus hsf_plugin_create(const HSFPluginContext* ctx,
                                              HSFPlugin** out_plugin,
                                              const HSFPluginVTable** out_vtable);

/* Must tolerate a plugin that was created but never started, and must have
 * stopped every thread and unsubscribed from every topic before returning. The
 * host may dlclose immediately afterwards, and a thread still running in
 * unmapped code is not a diagnosable crash. */
HSF_PLUGIN_EXPORT void hsf_plugin_destroy(HSFPlugin* plugin);

/* Boilerplate for the four exports. INFO_EXPR must be a pointer to storage with
 * static lifetime; CREATE_FN and DESTROY_FN match the signatures above. */
#define HSF_PLUGIN_DEFINE(INFO_EXPR, CREATE_FN, DESTROY_FN)                   \
  HSF_PLUGIN_EXPORT uint32_t hsf_plugin_abi_version(void) {                   \
    return HSF_PLUGIN_API_VERSION;                                            \
  }                                                                           \
  HSF_PLUGIN_EXPORT const HSFPluginInfo* hsf_plugin_get_info(void) {          \
    return (INFO_EXPR);                                                       \
  }                                                                           \
  HSF_PLUGIN_EXPORT HSFStatus hsf_plugin_create(                              \
      const HSFPluginContext* ctx, HSFPlugin** out_plugin,                    \
      const HSFPluginVTable** out_vtable) {                                   \
    if (!ctx || !out_plugin || !out_vtable) return HSF_ERR_INVALID_ARG;        \
    return CREATE_FN(ctx, out_plugin, out_vtable);                            \
  }                                                                           \
  HSF_PLUGIN_EXPORT void hsf_plugin_destroy(HSFPlugin* plugin) {              \
    if (plugin) DESTROY_FN(plugin);                                           \
  }

HSF_ABI_END

#endif /* HSF_PLUGIN_SDK_PLUGIN_H */
