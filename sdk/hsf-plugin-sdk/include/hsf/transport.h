/* HSF Plugin SDK — the Transport API, provided BY the host TO the plugin.
 *
 * A protocol driver says "send these bytes, tell me when more arrive". It must
 * not know whether they travel over a USB serial port, the main-board UART, TCP,
 * UDP, CAN, or a mock (plan §8). This is that boundary.
 *
 *
 * WHY THIS INTERFACE IS NON-BLOCKING, WHICH IS THE ONE DECISION HERE THAT
 * CANNOT BE WALKED BACK.
 *
 * A blocking `read(buf, n)` looks friendlier and is what the gateway's own
 * TcpSocket offers today. But a blocking read forces the caller to own a thread
 * to sit in it, so a blocking transport interface means one thread per driver,
 * forever — and once that shape is in the ABI, third-party plugins depend on it
 * and it cannot be changed without a major version break.
 *
 * The gateway already runs eight module threads plus a 16-thread Crow pool
 * (docs/architecture/current-state.md §5), against a stated target of 2 cores
 * and 256 MB with "avoid unnecessary threads" (plan §27). Wrapping today's
 * threaded drivers behind a blocking ABI would preserve that thread count
 * exactly and satisfy none of the constraint. So the contract is readiness-based
 * from the start: the HOST may run every driver on one event loop, and a driver
 * that wants a thread of its own can still have one.
 *
 * Drivers that genuinely want blocking semantics get them from
 * hsf/support.hpp's hsf::BlockingIo helper, which is a header-only C++
 * convenience built ON this interface. That keeps the ergonomics without
 * putting the thread requirement in the ABI.
 *
 *
 * READ AND WRITE RETURN VALUES, precisely — this is where non-blocking I/O is
 * usually got wrong:
 *
 *   HSF_OK        with *transferred > 0   progress was made
 *   HSF_OK        with *transferred == 0  on a STREAM, the peer closed cleanly;
 *                                         on a DATAGRAM, an empty datagram
 *   HSF_AGAIN     with *transferred == 0  would have blocked, nothing happened,
 *                                         retry after `wait`
 *   negative                              a real error; the transport may be
 *                                         unusable, check is_open
 *
 * Conflating HSF_AGAIN with a zero-length read is the classic bug: one means
 * "nothing yet", the other means "never again". They are separate values here
 * so the mistake is not expressible.
 */
#ifndef HSF_PLUGIN_SDK_TRANSPORT_H
#define HSF_PLUGIN_SDK_TRANSPORT_H

#include "hsf/error.h"

HSF_ABI_BEGIN

typedef enum {
  HSF_TRANSPORT_UNKNOWN = 0,
  HSF_TRANSPORT_SERIAL  = 1,   /* USB serial, main UART, RS232, RS485 */
  HSF_TRANSPORT_TCP     = 2,
  HSF_TRANSPORT_UDP     = 3,
  HSF_TRANSPORT_CAN     = 4,
  HSF_TRANSPORT_SPI     = 5,
  HSF_TRANSPORT_GPIO    = 6,
  /* Supplied by the SDK, not by hardware. Lets a driver's whole protocol be
   * tested with no device attached (plan §19). Deliberately in the same enum
   * as the real ones: a driver must not be able to tell, or the tests prove
   * nothing about production. */
  HSF_TRANSPORT_MOCK    = 100
} HSFTransportKind;

/* Readiness flags for `wait`, bitwise-OR. */
#define HSF_IO_READ   0x1u
#define HSF_IO_WRITE  0x2u
#define HSF_IO_ERROR  0x4u   /* output only; never ask for it */
#define HSF_IO_CLOSED 0x8u   /* output only */

/* Framing model, which the driver needs to know even though it does not care
 * how bytes travel: a Modbus RTU driver must scan for frame boundaries on a
 * stream, but on a datagram transport one receive is exactly one frame. */
typedef enum {
  HSF_FRAMING_STREAM   = 0,  /* serial, TCP — no message boundaries */
  HSF_FRAMING_DATAGRAM = 1   /* UDP, CAN — one read is one message */
} HSFFraming;

typedef struct HSFTransport HSFTransport; /* opaque, host-owned */

typedef struct {
  uint32_t struct_size;

  HSFTransportKind (*kind)(const HSFTransport* self);
  HSFFraming       (*framing)(const HSFTransport* self);

  /* Configuration comes from the host, out of the plugin's own config section
   * — the driver never parses a device path or a baud rate, and never sees
   * one. This is what plan §8 is really asking for. */
  HSFStatus (*open)(HSFTransport* self);
  void      (*close)(HSFTransport* self);
  int32_t   (*is_open)(const HSFTransport* self);

  /* Non-blocking. See the header comment for the exact return contract. */
  HSFStatus (*read)(HSFTransport* self, void* buf, size_t cap, size_t* transferred);
  HSFStatus (*write)(HSFTransport* self, const void* buf, size_t len, size_t* transferred);

  /* Block until one of `events` is ready, or `timeout_ms` elapses
   * (0 = poll, negative = wait indefinitely). Returns HSF_ERR_TIMEOUT on
   * expiry with *ready == 0.
   *
   * A driver running on the host's event loop never calls this — the host
   * waits for it and then calls the driver's on_readable. It exists for a
   * driver that owns its own thread, and for tests. */
  HSFStatus (*wait)(HSFTransport* self, uint32_t events, int32_t timeout_ms,
                    uint32_t* ready);

  /* Discard buffered input. Necessary after a protocol error on a shared RS485
   * bus, where the remains of someone else's reply would otherwise be parsed
   * as the start of ours. */
  HSFStatus (*flush_input)(HSFTransport* self);

  /* Underlying OS handle (fd on POSIX, SOCKET on Windows), or -1 when there is
   * none — which is the honest answer for a mock. Exposed ONLY so a host can
   * add it to its own poll set. A plugin that reads or writes it directly has
   * escaped the abstraction and will break on the next transport. */
  intptr_t (*native_handle)(const HSFTransport* self);

  /* Last transport-level error, borrowed, for logs. Never NULL-ptr'd. */
  HSFStr (*last_error)(const HSFTransport* self);
} HSFTransportVTable;

typedef struct {
  const HSFTransportVTable* vt;
  HSFTransport*             self;
} HSFTransportRef;

/* Host-side factory, so a driver can open a second connection at runtime
 * (a Modbus gateway fronting several RTU sub-buses) without knowing what it
 * is. `spec_json` matches the transport block of the plugin's own config, so a
 * driver passes through what it was given rather than constructing it. */
typedef struct HSFTransportFactory HSFTransportFactory;

typedef struct {
  uint32_t struct_size;
  HSFStatus (*create)(HSFTransportFactory* self, HSFStr spec_json,
                      HSFTransportRef* out);
  void      (*destroy)(HSFTransportFactory* self, HSFTransportRef transport);
  /* Whether this host can supply that kind at all, so a driver can fail at
   * initialize() with a clear message instead of at the first read. */
  int32_t   (*supports)(const HSFTransportFactory* self, HSFTransportKind kind);
} HSFTransportFactoryVTable;

typedef struct {
  const HSFTransportFactoryVTable* vt;
  HSFTransportFactory*             self;
} HSFTransportFactoryRef;

static inline int hsf_transport_valid(HSFTransportRef t) {
  return t.vt != NULL && t.self != NULL;
}

static inline const char* hsf_transport_kind_name(HSFTransportKind k) {
  switch (k) {
    case HSF_TRANSPORT_SERIAL: return "serial";
    case HSF_TRANSPORT_TCP:    return "tcp";
    case HSF_TRANSPORT_UDP:    return "udp";
    case HSF_TRANSPORT_CAN:    return "can";
    case HSF_TRANSPORT_SPI:    return "spi";
    case HSF_TRANSPORT_GPIO:   return "gpio";
    case HSF_TRANSPORT_MOCK:   return "mock";
    default:                   return "unknown";
  }
}

HSF_ABI_END

#endif /* HSF_PLUGIN_SDK_TRANSPORT_H */
