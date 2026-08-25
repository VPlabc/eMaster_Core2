/* HSF Plugin SDK — the event bus, provided BY the host TO the plugin.
 *
 * The gateway has no event bus today. Events propagate as direct callbacks:
 * SerialPort into a LuaRuntimeManager fan-out, ZK RTLog into a queue, PLC coil
 * transitions from a poll thread in main(). Every one of those paths names a
 * concrete module, which is why LuaRuntimeManager::Bind takes a fixed list of
 * module pointers (docs/architecture/current-state.md §3, §6).
 *
 * This is the replacement, and it is what actually decouples the Lua layer from
 * protocol implementations: a plugin publishes on a topic, the host fans out to
 * whatever is listening — Lua handlers, the WebSocket, the structured log —
 * without either side naming the other.
 *
 * THREADING CONTRACT, which is the part that bites. `publish` is safe to call
 * from any thread the plugin owns. A subscriber callback is invoked on the
 * HOST's dispatch thread, never re-entrantly inside publish, and must not
 * block: it runs on a thread shared with other subscribers. Anything slow gets
 * queued to the plugin's own context and done later.
 */
#ifndef HSF_PLUGIN_SDK_EVENT_H
#define HSF_PLUGIN_SDK_EVENT_H

#include "hsf/value.h"

HSF_ABI_BEGIN

typedef struct HSFEventBus HSFEventBus; /* opaque, host-owned */

/* Subscription handle. 0 is never valid, so it doubles as "not subscribed". */
typedef uint64_t HSFSubscription;

typedef struct {
  uint32_t struct_size;

  /* Dot-separated, lowercase, most-general-first:
   *   "driver.modbus.value"      a point changed
   *   "driver.zk.card"           a card was presented
   *   "transport.serial.closed"  a port dropped
   * The host may match a trailing "*" wildcard on subscribe. */
  HSFStr topic;

  /* Which plugin published it. Filled in by the host, not the publisher — a
   * plugin cannot attribute an event to someone else. */
  HSFStr source_plugin;

  /* JSON object, borrowed for the duration of the callback. JSON rather than a
   * typed struct because the whole point is that Core does not know what
   * protocols exist, and a tagged union would have to grow a case per
   * protocol — reintroducing exactly the coupling this removes. Use `value`
   * for the hot path instead. */
  HSFStr payload_json;

  /* Set when the event is a single point reading, so the common case costs no
   * JSON at all. kind == HSF_VALUE_NULL means "look at payload_json". */
  HSFValue value;

  int64_t timestamp_ms;
} HSFEvent;

typedef void (*HSFEventHandler)(const HSFEvent* ev, void* user);

typedef struct {
  uint32_t struct_size;

  /* Fire and forget. Never blocks on subscribers; the host queues. Returns
   * HSF_ERR_BUSY if its queue is full rather than growing without bound —
   * bounded queues are a §27 requirement, and a driver that can outrun the bus
   * needs to know. */
  HSFStatus (*publish)(HSFEventBus* self, HSFStr topic, HSFStr payload_json);

  /* Point-reading fast path: no JSON built, no JSON parsed. */
  HSFStatus (*publish_value)(HSFEventBus* self, HSFStr topic, HSFValue value);

  /* `topic_filter` may end in "*". Returns 0 on failure. */
  HSFSubscription (*subscribe)(HSFEventBus* self, HSFStr topic_filter,
                               HSFEventHandler handler, void* user);

  /* Idempotent. MUST have returned before the plugin frees anything the
   * handler could touch — the host guarantees no handler is running, and none
   * will start, once this returns. That guarantee is why unsubscribe exists
   * separately from destroy. */
  void (*unsubscribe)(HSFEventBus* self, HSFSubscription sub);
} HSFEventBusVTable;

typedef struct {
  const HSFEventBusVTable* vt;
  HSFEventBus*             self;
} HSFEventBusRef;

static inline HSFStatus hsf_publish_value(HSFEventBusRef bus, const char* topic,
                                          HSFValue v) {
  if (!bus.vt || !bus.vt->publish_value) return HSF_ERR_NOT_SUPPORTED;
  return bus.vt->publish_value(bus.self, hsf_cstr(topic), v);
}

HSF_ABI_END

#endif /* HSF_PLUGIN_SDK_EVENT_H */
