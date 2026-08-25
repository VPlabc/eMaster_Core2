/* HSF Plugin SDK — status codes and borrowed strings.
 *
 * Every fallible ABI call returns HSFStatus. Nothing throws: an exception
 * unwinding across a shared-object boundary between two translation units that
 * may have been built by different compilers is undefined behaviour, so a
 * plugin MUST catch everything at its entry points. hsf_guard() in support.hpp
 * does that for C++ plugins.
 */
#ifndef HSF_PLUGIN_SDK_ERROR_H
#define HSF_PLUGIN_SDK_ERROR_H

#include "hsf/version.h"

HSF_ABI_BEGIN

typedef int32_t HSFStatus;

/* 0 is success and negatives are failures, so `if (st < 0)` is the error test
 * and HSF_AGAIN can be positive without being an error. */
#define HSF_OK 0

/* Not an error. The operation would have blocked and did nothing; retry when
 * the transport signals readiness. Distinct from HSF_OK-with-zero-bytes, which
 * on a stream means the peer closed. Getting this distinction wrong is the
 * classic non-blocking I/O bug, so the two are separate values. */
#define HSF_AGAIN 1

#define HSF_ERR_UNKNOWN          (-1)
#define HSF_ERR_INVALID_ARG      (-2)
#define HSF_ERR_NOT_SUPPORTED    (-3)   /* honest refusal, not a silent no-op */
#define HSF_ERR_NOT_OPEN         (-4)
#define HSF_ERR_ALREADY_OPEN     (-5)
#define HSF_ERR_TIMEOUT          (-6)
#define HSF_ERR_IO               (-7)
#define HSF_ERR_CLOSED           (-8)   /* peer went away mid-operation */
#define HSF_ERR_PROTOCOL         (-9)   /* malformed frame, bad CRC, bad reply */
#define HSF_ERR_NO_MEMORY        (-10)
#define HSF_ERR_NOT_FOUND        (-11)
#define HSF_ERR_PERMISSION       (-12)  /* undeclared permission (plan §14) */
#define HSF_ERR_CONFIG           (-13)
#define HSF_ERR_STATE            (-14)  /* wrong lifecycle state for this call */
#define HSF_ERR_VERSION          (-15)  /* incompatible Plugin API version */
#define HSF_ERR_BUSY             (-16)
#define HSF_ERR_INTERNAL         (-17)

/* A borrowed, NOT necessarily NUL-terminated string.
 *
 * Borrowed means: valid only until the call that produced it returns, or as
 * documented at the specific slot. The receiver copies if it wants to keep it.
 * This is the only string form that crosses the ABI — no allocator is shared
 * between host and plugin, so neither side may free the other's memory. */
typedef struct {
  const char* ptr;
  size_t      len;
} HSFStr;

/* Literal helper. sizeof-1 rather than strlen so it stays a compile-time
 * constant and can initialise something with static storage duration.
 *
 * Two spellings because there is no one spelling that is valid in both
 * languages: C needs a compound literal, which is not standard C++ (GCC and
 * Clang accept it as an extension, MSVC does not — and the gateway builds with
 * MSVC for Windows x86). The C++ form is braced aggregate initialisation,
 * which is a constant expression there. */
#if defined(__cplusplus)
#  define HSF_STR_LIT(s) (HSFStr{ (s), sizeof(s) - 1 })
#else
#  define HSF_STR_LIT(s) { (s), sizeof(s) - 1 }
#endif

static inline HSFStr hsf_str_empty(void) {
  HSFStr s;
  s.ptr = NULL;
  s.len = 0;
  return s;
}

/* Wrap a NUL-terminated C string. Lives here beside HSFStr rather than in
 * config.h, because every header that passes a string needs it. */
static inline HSFStr hsf_cstr(const char* s) {
  HSFStr r;
  size_t n = 0;
  if (!s) return hsf_str_empty();
  while (s[n] != '\0') ++n;
  r.ptr = s;
  r.len = n;
  return r;
}

/* Human-readable name for a status. Static storage, always non-NULL, safe to
 * log. Deliberately a static inline in the header rather than a library
 * function: a plugin must be able to describe an error without linking against
 * anything, including on the path where loading the host's library failed. */
static inline const char* hsf_status_name(HSFStatus st) {
  switch (st) {
    case HSF_OK:                   return "OK";
    case HSF_AGAIN:                return "AGAIN";
    case HSF_ERR_INVALID_ARG:      return "INVALID_ARG";
    case HSF_ERR_NOT_SUPPORTED:    return "NOT_SUPPORTED";
    case HSF_ERR_NOT_OPEN:         return "NOT_OPEN";
    case HSF_ERR_ALREADY_OPEN:     return "ALREADY_OPEN";
    case HSF_ERR_TIMEOUT:          return "TIMEOUT";
    case HSF_ERR_IO:               return "IO";
    case HSF_ERR_CLOSED:           return "CLOSED";
    case HSF_ERR_PROTOCOL:         return "PROTOCOL";
    case HSF_ERR_NO_MEMORY:        return "NO_MEMORY";
    case HSF_ERR_NOT_FOUND:        return "NOT_FOUND";
    case HSF_ERR_PERMISSION:       return "PERMISSION";
    case HSF_ERR_CONFIG:           return "CONFIG";
    case HSF_ERR_STATE:            return "STATE";
    case HSF_ERR_VERSION:          return "VERSION";
    case HSF_ERR_BUSY:             return "BUSY";
    case HSF_ERR_INTERNAL:         return "INTERNAL";
    case HSF_ERR_UNKNOWN:          return "UNKNOWN";
    default:                       return "UNRECOGNISED";
  }
}

HSF_ABI_END

#endif /* HSF_PLUGIN_SDK_ERROR_H */
