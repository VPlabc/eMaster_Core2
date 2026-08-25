/* HSF Plugin SDK — the value type that crosses the Device API.
 *
 * One tagged union, deliberately small and trivially copyable, so a read or
 * write costs no allocation. This is the type the plan's §15 device
 * abstraction moves around: `device.read("PLC1", "temperature")` yields an
 * HSFValue whether the point behind it is a Modbus holding register, a BACnet
 * analog-input present-value, or a mock.
 *
 * BLOB and STR borrow their bytes; they do not own them. A driver returning
 * either must keep the storage alive until the call that returned it has been
 * consumed by the host — in practice, a buffer owned by the driver instance.
 * Nothing here is ever freed across the boundary, because host and plugin do
 * not share an allocator.
 */
#ifndef HSF_PLUGIN_SDK_VALUE_H
#define HSF_PLUGIN_SDK_VALUE_H

#include "hsf/error.h"

HSF_ABI_BEGIN

typedef enum {
  HSF_VALUE_NULL = 0,   /* no reading available; distinct from a zero reading */
  HSF_VALUE_BOOL = 1,
  HSF_VALUE_I64  = 2,
  HSF_VALUE_U64  = 3,
  HSF_VALUE_F64  = 4,
  HSF_VALUE_STR  = 5,   /* borrowed */
  HSF_VALUE_BLOB = 6    /* borrowed */
} HSFValueKind;

typedef struct {
  const uint8_t* ptr;
  size_t         len;
} HSFBlob;

typedef struct {
  uint32_t     struct_size;
  HSFValueKind kind;

  /* Milliseconds since the Unix epoch, UTC, or 0 when the driver has no
   * trustworthy clock for this reading. Carried WITH the value rather than
   * taken by the host on receipt: a value read from a device buffer may be
   * seconds old, and for access-control events the difference matters. */
  int64_t timestamp_ms;

  union {
    int32_t b;      /* HSF_VALUE_BOOL — int32, not bool: _Bool has no fixed
                     * ABI size across compilers */
    int64_t i64;
    uint64_t u64;
    double  f64;
    HSFStr  str;
    HSFBlob blob;
  } as;
} HSFValue;

/* Constructors. static inline so they cost nothing and need no library. */

static inline HSFValue hsf_value_null(void) {
  HSFValue v;
  v.struct_size = (uint32_t)sizeof(HSFValue);
  v.kind = HSF_VALUE_NULL;
  v.timestamp_ms = 0;
  v.as.u64 = 0;
  return v;
}

static inline HSFValue hsf_value_bool(int32_t b) {
  HSFValue v = hsf_value_null();
  v.kind = HSF_VALUE_BOOL;
  v.as.b = b ? 1 : 0;
  return v;
}

static inline HSFValue hsf_value_i64(int64_t i) {
  HSFValue v = hsf_value_null();
  v.kind = HSF_VALUE_I64;
  v.as.i64 = i;
  return v;
}

static inline HSFValue hsf_value_u64(uint64_t u) {
  HSFValue v = hsf_value_null();
  v.kind = HSF_VALUE_U64;
  v.as.u64 = u;
  return v;
}

static inline HSFValue hsf_value_f64(double d) {
  HSFValue v = hsf_value_null();
  v.kind = HSF_VALUE_F64;
  v.as.f64 = d;
  return v;
}

static inline HSFValue hsf_value_str(const char* ptr, size_t len) {
  HSFValue v = hsf_value_null();
  v.kind = HSF_VALUE_STR;
  v.as.str.ptr = ptr;
  v.as.str.len = len;
  return v;
}

static inline HSFValue hsf_value_blob(const uint8_t* ptr, size_t len) {
  HSFValue v = hsf_value_null();
  v.kind = HSF_VALUE_BLOB;
  v.as.blob.ptr = ptr;
  v.as.blob.len = len;
  return v;
}

static inline const char* hsf_value_kind_name(HSFValueKind k) {
  switch (k) {
    case HSF_VALUE_NULL: return "null";
    case HSF_VALUE_BOOL: return "bool";
    case HSF_VALUE_I64:  return "i64";
    case HSF_VALUE_U64:  return "u64";
    case HSF_VALUE_F64:  return "f64";
    case HSF_VALUE_STR:  return "str";
    case HSF_VALUE_BLOB: return "blob";
    default:             return "?";
  }
}

/* The wire/register encoding of a point, named independently of any one
 * protocol so a device mapping can say "uint16, big-endian" without the
 * mapping layer knowing what Modbus is. */
typedef enum {
  HSF_ENC_NONE = 0,
  HSF_ENC_BIT,
  HSF_ENC_I16, HSF_ENC_U16,
  HSF_ENC_I32, HSF_ENC_U32,
  HSF_ENC_I64, HSF_ENC_U64,
  HSF_ENC_F32, HSF_ENC_F64,
  HSF_ENC_STRING,
  HSF_ENC_RAW
} HSFEncoding;

typedef enum {
  HSF_BYTE_ORDER_NATIVE = 0,
  HSF_BYTE_ORDER_BIG,
  HSF_BYTE_ORDER_LITTLE,
  /* The two 32-bit register swaps that every real Modbus deployment
   * eventually needs and that no amount of wishing makes uniform. */
  HSF_BYTE_ORDER_BIG_WORD_SWAP,
  HSF_BYTE_ORDER_LITTLE_WORD_SWAP
} HSFByteOrder;

HSF_ABI_END

#endif /* HSF_PLUGIN_SDK_VALUE_H */
