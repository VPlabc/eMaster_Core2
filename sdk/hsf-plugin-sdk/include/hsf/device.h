/* HSF Plugin SDK — device addressing.
 *
 * The plan's §15 goal: application code says
 *
 *     device.read("PLC1", "temperature")
 *
 * not
 *
 *     modbus.read("PLC1", 40001, "uint16")
 *
 * so the same Lua logic survives the protocol changing underneath it. The
 * mapping from a point NAME to a protocol-specific location is configuration,
 * held by the host; a driver is handed the resolved location and never sees the
 * name unless it wants it for logging.
 *
 * WHY THE LOCATION IS AN OPAQUE STRING PLUS THREE INTEGERS. Every protocol
 * addresses differently: Modbus wants a table and a register, BACnet wants an
 * object type, instance and property, ZK wants a door and an operation. A
 * tagged union with a case per protocol would put every protocol's shape into
 * Core — which is the coupling this architecture removes. So Core carries the
 * address as data it does not interpret, and the driver that owns the protocol
 * is the only code that parses it.
 *
 * The integers are not redundant with the string: they are the hot path. A
 * Modbus poll loop reading 200 points must not re-parse "40001" 200 times a
 * second on a 256 MB device, so the host resolves numeric parts once at
 * configuration time and the driver reads them directly.
 */
#ifndef HSF_PLUGIN_SDK_DEVICE_H
#define HSF_PLUGIN_SDK_DEVICE_H

#include "hsf/value.h"

HSF_ABI_BEGIN

typedef struct {
  uint32_t struct_size;

  /* Operator-facing names, borrowed. Present for logs and for a driver that
   * genuinely keys on them; a driver should prefer the numeric fields. */
  HSFStr device_id;   /* "PLC1"        */
  HSFStr point_name;  /* "temperature" */

  /* Protocol-specific location, verbatim from the point's configuration.
   * Modbus: "holding:40001". BACnet: "analog-input:1:present-value".
   * Parsed by the owning driver, never by Core. */
  HSFStr location;

  /* Pre-resolved numeric parts, meaning assigned by the driver. Modbus uses
   * a1 = table, a2 = register, a3 = count. BACnet uses a1 = object type,
   * a2 = instance, a3 = property id. Zero when unused. */
  int64_t a1;
  int64_t a2;
  int64_t a3;

  /* Unit id / slave address / node id — the sub-device selector nearly every
   * bus protocol has and that is never part of the point address proper. */
  int32_t unit;

  /* How to decode the bytes at that location. */
  HSFEncoding  encoding;
  HSFByteOrder byte_order;

  /* Applied by the HOST after a read and inverted before a write, so scaling
   * lives in configuration rather than being reimplemented per driver. A
   * driver returns raw engineering units and ignores both. scale == 0 is
   * treated as 1 — a zero multiplier would silently destroy every reading, so
   * it is read as "unset" rather than obeyed. */
  double scale;
  double offset;
} HSFDeviceAddress;

/* Access intent, so a driver can reject a write to a read-only point without
 * attempting it and reporting a protocol error. */
typedef enum {
  HSF_ACCESS_READ       = 0x1,
  HSF_ACCESS_WRITE      = 0x2,
  HSF_ACCESS_READ_WRITE = 0x3
} HSFAccess;

/* One point as a driver declares it during discovery — the answer to "what
 * does this device have?", which is what the Test Tool's scan needs and what
 * saves an operator typing a point map by hand. */
typedef struct {
  uint32_t struct_size;
  HSFStr      point_name;
  HSFStr      location;
  HSFStr      unit_text;    /* "degC", "kPa" — display only */
  HSFEncoding encoding;
  HSFAccess   access;
} HSFPointInfo;

static inline HSFDeviceAddress hsf_device_address_init(void) {
  HSFDeviceAddress a;
  a.struct_size = (uint32_t)sizeof(HSFDeviceAddress);
  a.device_id = hsf_str_empty();
  a.point_name = hsf_str_empty();
  a.location = hsf_str_empty();
  a.a1 = 0;
  a.a2 = 0;
  a.a3 = 0;
  a.unit = 0;
  a.encoding = HSF_ENC_NONE;
  a.byte_order = HSF_BYTE_ORDER_NATIVE;
  a.scale = 1.0;
  a.offset = 0.0;
  return a;
}

HSF_ABI_END

#endif /* HSF_PLUGIN_SDK_DEVICE_H */
