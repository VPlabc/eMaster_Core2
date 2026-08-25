/* HSF Plugin SDK — the Driver API, provided BY the plugin TO the host.
 *
 * Everything above is a service the host hands down. This is the one the plugin
 * implements and hands up. It is the C-compatible form of the plan's §7 IDriver
 * sketch: same seven capabilities, expressed as a vtable plus an opaque
 * instance so no C++ type crosses the boundary.
 *
 *
 * LIFECYCLE, and the state each call is legal in:
 *
 *   created  --initialize-->  ready  --start-->  running
 *                              ^                   |
 *                              +------ stop -------+
 *
 *   initialize   once, from `created`. Parse configuration, validate it, claim
 *                nothing. MUST NOT touch hardware: it runs while the operator
 *                is still editing settings, and a driver that opens a port here
 *                makes a bad config unrecoverable without a restart.
 *   start        `ready` -> `running`. Open the transport, begin polling.
 *   stop         `running` -> `ready`. Idempotent. Must return promptly — the
 *                host calls it during shutdown and on plugin disable, and a
 *                stop that blocks is indistinguishable from a hang.
 *   read/write   `running` only; HSF_ERR_STATE otherwise.
 *   status       any state, including after a failure. Never blocks.
 *
 * A driver may be initialized, started, stopped and started again without being
 * destroyed — that is what the Enable/Disable buttons do, and it is why start
 * must not assume a virgin instance.
 */
#ifndef HSF_PLUGIN_SDK_DRIVER_H
#define HSF_PLUGIN_SDK_DRIVER_H

#include "hsf/config.h"
#include "hsf/device.h"
#include "hsf/transport.h"

HSF_ABI_BEGIN

typedef enum {
  HSF_DRIVER_CREATED = 0,
  HSF_DRIVER_READY   = 1,
  HSF_DRIVER_RUNNING = 2,
  HSF_DRIVER_FAILED  = 3
} HSFDriverState;

typedef struct {
  uint32_t       struct_size;
  HSFDriverState state;

  /* Whether the device is actually reachable right now. Deliberately separate
   * from `state`: a driver can be RUNNING and correct while the PLC at the far
   * end is unplugged, and the dashboard needs to tell those apart. */
  int32_t connected;

  /* Borrowed, valid until the next call on this driver. Empty when healthy.
   * This is where the reason a device is unreachable belongs — and it must be
   * the reason THIS driver knows, not a generic code. */
  HSFStr last_error;

  /* Cheap counters for the dashboard and for spotting a flapping link. */
  uint64_t reads_ok;
  uint64_t reads_failed;
  uint64_t writes_ok;
  uint64_t writes_failed;
  int64_t  connected_since_ms;  /* 0 when not connected */
} HSFDriverStatus;

typedef struct HSFDriver HSFDriver; /* opaque, plugin-owned */

typedef struct {
  uint32_t struct_size;

  /* `transport` may be an invalid ref (vt == NULL) for a driver that needs
   * none — an MQTT bridge over the host's own client, say. Check with
   * hsf_transport_valid before using it. */
  HSFStatus (*initialize)(HSFDriver* self, HSFConfigRef config,
                          HSFTransportRef transport);

  HSFStatus (*start)(HSFDriver* self);
  HSFStatus (*stop)(HSFDriver* self);

  HSFStatus (*read)(HSFDriver* self, const HSFDeviceAddress* addr, HSFValue* out);
  HSFStatus (*write)(HSFDriver* self, const HSFDeviceAddress* addr,
                     const HSFValue* value);

  /* Batch forms. A protocol that can fetch 100 registers in one request must
   * not be forced into 100 round trips — on a 9600-baud RS485 bus that is the
   * difference between a 200 ms scan and a 20 s one.
   *
   * Per-point results go in `statuses` so ONE bad point does not fail the
   * batch; the return value covers the request as a whole. May be NULL, and
   * the host then falls back to looping `read`. */
  HSFStatus (*read_many)(HSFDriver* self, const HSFDeviceAddress* addrs,
                         size_t count, HSFValue* out, HSFStatus* statuses);
  HSFStatus (*write_many)(HSFDriver* self, const HSFDeviceAddress* addrs,
                          const HSFValue* values, size_t count,
                          HSFStatus* statuses);

  void (*status)(const HSFDriver* self, HSFDriverStatus* out);

  /* Active reachability probe, for the health check in the install workflow
   * (plan §13) and the Test Tool's Connect button. Distinct from `status`,
   * which only reports what is already known and never talks to the device. */
  HSFStatus (*health)(HSFDriver* self);

  /* --- optional slots: NULL is a valid, complete answer --- */

  /* Enumerate what the device has, for the Test Tool's scan and for building a
   * point map without typing one. Writes up to `cap` entries and always sets
   * *found to the true total, so a caller can size a buffer and retry. */
  HSFStatus (*discover)(HSFDriver* self, HSFPointInfo* out, size_t cap,
                        size_t* found);

  /* Called by the host's event loop when the transport says it is readable.
   * A driver that implements this needs NO THREAD OF ITS OWN — this is the slot
   * that makes the §27 single-event-loop design possible, and the reason the
   * transport interface is non-blocking. Leave it NULL and own a thread
   * instead; both are supported, but only one of them scales to 256 MB. */
  HSFStatus (*on_readable)(HSFDriver* self);

  /* Periodic tick, if the driver asked for one via HSFDriverInfo::tick_ms.
   * Also runs on the host's loop, so it must not block. */
  HSFStatus (*on_tick)(HSFDriver* self);

  /* Protocol-specific escape hatch, for the operations a generic Driver API
   * genuinely cannot express — opening a door, cancelling an alarm, restarting
   * a panel. Keyed by a string the driver documents; `args_json` in,
   * `result_json` out (borrowed, valid until the next call).
   *
   * This is how zk.openDoor survives the migration without Core learning what
   * a door is. Resist putting anything here that read/write can express. */
  HSFStatus (*command)(HSFDriver* self, HSFStr name, HSFStr args_json,
                       HSFStr* result_json);
} HSFDriverVTable;

typedef struct {
  const HSFDriverVTable* vt;
  HSFDriver*             self;
} HSFDriverRef;

/* What a driver declares about itself at creation, so the host can schedule it
 * correctly without calling anything. */
typedef struct {
  uint32_t struct_size;

  /* Which transports this driver can work over. A host asked to pair it with
   * anything else rejects the combination at configuration time, with a message
   * naming both, rather than at the first read. */
  const HSFTransportKind* supported_transports;
  size_t                  supported_transport_count;

  /* Requested tick period; 0 means no tick wanted. The host may lengthen it
   * under load — this is a request, not a guarantee. */
  uint32_t tick_ms;

  /* Non-zero when the driver has its own thread and does NOT want the host's
   * event loop to call on_readable. Declaring this honestly matters: a driver
   * that owns a thread AND gets called from the loop has two threads in its
   * own state. */
  int32_t owns_thread;

  /* Largest single frame, so the host can size a shared buffer once instead of
   * every driver allocating its own. 0 means "use the host default". */
  uint32_t max_frame_bytes;
} HSFDriverInfo;

HSF_ABI_END

#endif /* HSF_PLUGIN_SDK_DRIVER_H */
