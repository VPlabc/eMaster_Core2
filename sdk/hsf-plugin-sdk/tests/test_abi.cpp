/* ABI contract tests.
 *
 * These check the promises the SDK makes to plugin authors that, if broken,
 * break every plugin at once and cannot be fixed without a major version bump:
 * version compatibility arithmetic, struct_size self-description, POD-ness of
 * everything that crosses the boundary, and the exception guard.
 */
#include "hsf/plugin.h"
#include "hsf/support.hpp"
#include "hsf/testing.hpp"

#include <stdexcept>
#include <string>
#include <type_traits>

HSF_TEST("version: packing and unpacking round-trip") {
  HSF_CHECK_EQ(HSF_API_VERSION_MAJOR_OF(HSF_PLUGIN_API_VERSION),
               (uint32_t)HSF_PLUGIN_API_VERSION_MAJOR);
  HSF_CHECK_EQ(HSF_API_VERSION_MINOR_OF(HSF_PLUGIN_API_VERSION),
               (uint32_t)HSF_PLUGIN_API_VERSION_MINOR);
}

HSF_TEST("version: compatibility is asymmetric in minor, exact in major") {
  const uint32_t v1_0 = (1u << 16) | 0u;
  const uint32_t v1_1 = (1u << 16) | 1u;
  const uint32_t v1_9 = (1u << 16) | 9u;
  const uint32_t v2_0 = (2u << 16) | 0u;

  /* Same version: obviously fine. */
  HSF_CHECK(HSF_API_VERSION_COMPATIBLE(v1_0, v1_0));

  /* Newer host, older plugin: fine. Minor bumps only append. */
  HSF_CHECK(HSF_API_VERSION_COMPATIBLE(v1_9, v1_0));
  HSF_CHECK(HSF_API_VERSION_COMPATIBLE(v1_1, v1_0));

  /* Older host, newer plugin: REFUSED. The plugin may call a vtable slot this
   * host does not have, and the failure would be a jump through a null
   * pointer, not a clean error. */
  HSF_CHECK(!HSF_API_VERSION_COMPATIBLE(v1_0, v1_1));

  /* Major mismatch: refused in both directions. */
  HSF_CHECK(!HSF_API_VERSION_COMPATIBLE(v1_9, v2_0));
  HSF_CHECK(!HSF_API_VERSION_COMPATIBLE(v2_0, v1_9));
}

HSF_TEST("abi: every boundary struct is trivially copyable POD") {
  /* If any of these gains a constructor, a destructor, or a virtual, it stops
   * being safe to memcpy across a shared-object boundary and the ABI is
   * silently broken. The compiler should be the one to notice. */
  HSF_CHECK(std::is_trivially_copyable<HSFStr>::value);
  HSF_CHECK(std::is_trivially_copyable<HSFValue>::value);
  HSF_CHECK(std::is_trivially_copyable<HSFDeviceAddress>::value);
  HSF_CHECK(std::is_trivially_copyable<HSFDriverStatus>::value);
  HSF_CHECK(std::is_trivially_copyable<HSFPluginInfo>::value);
  HSF_CHECK(std::is_trivially_copyable<HSFPluginContext>::value);
  HSF_CHECK(std::is_trivially_copyable<HSFEvent>::value);
  HSF_CHECK(std::is_trivially_copyable<HSFDriverInfo>::value);
  HSF_CHECK(std::is_trivially_copyable<HSFPointInfo>::value);

  HSF_CHECK(std::is_standard_layout<HSFStr>::value);
  HSF_CHECK(std::is_standard_layout<HSFValue>::value);
  HSF_CHECK(std::is_standard_layout<HSFPluginInfo>::value);
  HSF_CHECK(std::is_standard_layout<HSFPluginContext>::value);
}

HSF_TEST("abi: struct_size is the first member of every versioned struct") {
  /* The append-only growth rule depends on this: a reader checks struct_size
   * before touching anything, which only works if struct_size itself is at a
   * known offset that never moves. Zero is the only offset that qualifies. */
  HSF_CHECK_EQ(offsetof(HSFValue, struct_size), (size_t)0);
  HSF_CHECK_EQ(offsetof(HSFDeviceAddress, struct_size), (size_t)0);
  HSF_CHECK_EQ(offsetof(HSFDriverStatus, struct_size), (size_t)0);
  HSF_CHECK_EQ(offsetof(HSFPluginInfo, struct_size), (size_t)0);
  HSF_CHECK_EQ(offsetof(HSFPluginContext, struct_size), (size_t)0);
  HSF_CHECK_EQ(offsetof(HSFDriverVTable, struct_size), (size_t)0);
  HSF_CHECK_EQ(offsetof(HSFTransportVTable, struct_size), (size_t)0);
  HSF_CHECK_EQ(offsetof(HSFPluginVTable, struct_size), (size_t)0);
  HSF_CHECK_EQ(offsetof(HSFEvent, struct_size), (size_t)0);
}

HSF_TEST("abi: HSFStr is a plain two-word borrow") {
  /* Size is asserted because HSFStr is in every signature; if it silently grew
   * a third field, plugins built against the old header would misread it. */
  HSF_CHECK_EQ(sizeof(HSFStr), sizeof(const char*) + sizeof(size_t));
  HSFStr e = hsf_str_empty();
  HSF_CHECK(e.ptr == nullptr);
  HSF_CHECK_EQ(e.len, (size_t)0);

  HSFStr lit = HSF_STR_LIT("abcd");
  HSF_CHECK_EQ(lit.len, (size_t)4);   /* not 5: no NUL counted */

  HSFStr c = hsf_cstr("abcde");
  HSF_CHECK_EQ(c.len, (size_t)5);
  HSF_CHECK_EQ(hsf_cstr(nullptr).len, (size_t)0);
}

HSF_TEST("value: constructors set kind, size and a null timestamp") {
  HSFValue v = hsf_value_i64(-7);
  HSF_CHECK_EQ((int)v.kind, (int)HSF_VALUE_I64);
  HSF_CHECK_EQ(v.as.i64, (int64_t)-7);
  HSF_CHECK_EQ(v.struct_size, (uint32_t)sizeof(HSFValue));
  HSF_CHECK_EQ(v.timestamp_ms, (int64_t)0);

  HSF_CHECK_EQ((int)hsf_value_null().kind, (int)HSF_VALUE_NULL);
  HSF_CHECK_EQ(hsf_value_bool(5).as.b, 1);      /* normalised to 0/1 */
  HSF_CHECK_EQ(hsf_value_bool(0).as.b, 0);
  HSF_CHECK_EQ(hsf_value_f64(1.5).as.f64, 1.5);
  HSF_CHECK_EQ(hsf_value_u64(9u).as.u64, (uint64_t)9);
}

HSF_TEST("value: a NULL value is distinct from a zero reading") {
  /* A dashboard must be able to show "no data" differently from "0.0", or a
   * dead sensor looks like a cold room. */
  HSFValue absent = hsf_value_null();
  HSFValue zero = hsf_value_f64(0.0);
  HSF_CHECK_NE((int)absent.kind, (int)zero.kind);
}

HSF_TEST("device: the default address scales by 1, not by 0") {
  /* scale defaulting to 0 would multiply every reading to nothing — a silent,
   * total data loss that looks like working code. */
  HSFDeviceAddress a = hsf_device_address_init();
  HSF_CHECK_EQ(a.scale, 1.0);
  HSF_CHECK_EQ(a.offset, 0.0);
  HSF_CHECK_EQ((int)a.encoding, (int)HSF_ENC_NONE);
  HSF_CHECK_EQ((int)a.byte_order, (int)HSF_BYTE_ORDER_NATIVE);
  HSF_CHECK_EQ(a.struct_size, (uint32_t)sizeof(HSFDeviceAddress));
}

HSF_TEST("status: AGAIN is not an error, everything negative is") {
  HSF_CHECK(HSF_OK == 0);
  HSF_CHECK(HSF_AGAIN > 0);       /* so `st < 0` is the error test */
  HSF_CHECK(HSF_ERR_IO < 0);
  HSF_CHECK(HSF_ERR_TIMEOUT < 0);
  HSF_CHECK(HSF_ERR_NOT_SUPPORTED < 0);
}

HSF_TEST("status: every code has a name and none is null") {
  const HSFStatus all[] = {
      HSF_OK, HSF_AGAIN, HSF_ERR_UNKNOWN, HSF_ERR_INVALID_ARG,
      HSF_ERR_NOT_SUPPORTED, HSF_ERR_NOT_OPEN, HSF_ERR_ALREADY_OPEN,
      HSF_ERR_TIMEOUT, HSF_ERR_IO, HSF_ERR_CLOSED, HSF_ERR_PROTOCOL,
      HSF_ERR_NO_MEMORY, HSF_ERR_NOT_FOUND, HSF_ERR_PERMISSION,
      HSF_ERR_CONFIG, HSF_ERR_STATE, HSF_ERR_VERSION, HSF_ERR_BUSY,
      HSF_ERR_INTERNAL};
  for (HSFStatus st : all) {
    const char* n = hsf_status_name(st);
    HSF_CHECK(n != nullptr);
    HSF_CHECK(std::string(n) != "UNRECOGNISED");
  }
  /* A code nobody defined still returns something printable. */
  HSF_CHECK_EQ(std::string(hsf_status_name(-9999)), std::string("UNRECOGNISED"));
}

HSF_TEST("guard: exceptions become status codes and never escape") {
  /* This is what stops one plugin's bad std::stoi from terminating the whole
   * gateway. */
  HSF_CHECK_STATUS(hsf::Guard([]() -> HSFStatus { return HSF_OK; }), HSF_OK);
  HSF_CHECK_STATUS(hsf::Guard([]() -> HSFStatus {
                     throw std::runtime_error("boom");
                   }),
                   HSF_ERR_INTERNAL);
  HSF_CHECK_STATUS(hsf::Guard([]() -> HSFStatus { throw std::bad_alloc(); }),
                   HSF_ERR_NO_MEMORY);
  HSF_CHECK_STATUS(hsf::Guard([]() -> HSFStatus { throw 42; }), HSF_ERR_UNKNOWN);

  bool ran = false;
  hsf::GuardVoid([&] { ran = true; throw std::runtime_error("ignored"); });
  HSF_CHECK(ran);   /* returned normally despite throwing */
}

HSF_TEST("support: ToString and Borrow round-trip, and empties are safe") {
  const std::string s = "hello";
  HSFStr b = hsf::Borrow(s);
  HSF_CHECK_EQ(b.len, (size_t)5);
  HSF_CHECK_EQ(hsf::ToString(b), s);
  HSF_CHECK_EQ(hsf::ToString(hsf_str_empty()), std::string());
}

HSF_TEST("logging: calls through a zeroed ref do not crash") {
  /* A plugin that failed early holds a zeroed context and will still try to
   * log — which is precisely when a crash is least diagnosable. */
  HSFLoggerRef none{};
  hsf_log(none, HSF_LOG_ERROR, "must not crash");
  HSF_CHECK_EQ(hsf_log_enabled(none, HSF_LOG_ERROR), 0);
}

HSF_TEST("config: a zeroed ref returns every fallback") {
  HSFConfigRef none{};
  HSF_CHECK_EQ(hsf_cfg_i64(none, "k", 7), (int64_t)7);
  HSF_CHECK_EQ(hsf_cfg_f64(none, "k", 1.25), 1.25);
  HSF_CHECK_EQ(hsf_cfg_bool(none, "k", 1), 1);
  HSF_CHECK_EQ(hsf_cfg_has(none, "k"), 0);
  HSF_CHECK_EQ(hsf::ToString(hsf_cfg_str(none, "k", "def")), std::string("def"));
}

HSF_TEST("transport: an invalid ref is detected before use") {
  HSFTransportRef bad{};
  HSF_CHECK_EQ(hsf_transport_valid(bad), 0);
  HSF_CHECK_EQ(std::string(hsf_transport_kind_name(HSF_TRANSPORT_MOCK)),
               std::string("mock"));
  HSF_CHECK_EQ(std::string(hsf_transport_kind_name(HSF_TRANSPORT_SERIAL)),
               std::string("serial"));
}

HSF_TEST("platform: the triple is set and not the unknown fallback") {
  const std::string t = HSF_PLATFORM_TRIPLE;
  HSF_CHECK(!t.empty());
  HSF_CHECK_NE(t, std::string("unknown"));
}

HSF_TEST_MAIN()
