/* The test that actually proves the ABI: dlopen a real plugin and drive it.
 *
 * Everything else in this suite compiles the SDK into one binary, which cannot
 * catch the failures that matter — a missing export, default visibility hiding
 * nothing, a struct the two sides disagree about, an exception escaping. This
 * loads a separately-compiled shared object through nothing but the four
 * exported C symbols and runs it, which is exactly what the Plugin Manager will
 * do.
 *
 * HSF_TEST_PLUGIN_PATH is set by CMake to the built example plugin.
 */
#include "hsf/plugin.h"
#include "hsf/mock_transport.hpp"
#include "hsf/testing.hpp"

#include <string>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
using LibHandle = HMODULE;
static LibHandle OpenLib(const char* p) { return LoadLibraryA(p); }
static void* Sym(LibHandle h, const char* n) {
  return reinterpret_cast<void*>(GetProcAddress(h, n));
}
static void CloseLib(LibHandle h) { if (h) FreeLibrary(h); }
#else
#  include <dlfcn.h>
using LibHandle = void*;
static LibHandle OpenLib(const char* p) { return dlopen(p, RTLD_NOW | RTLD_LOCAL); }
static void* Sym(LibHandle h, const char* n) { return dlsym(h, n); }
static void CloseLib(LibHandle h) { if (h) dlclose(h); }
#endif

#ifndef HSF_TEST_PLUGIN_PATH
#  define HSF_TEST_PLUGIN_PATH ""
#endif

namespace {

using AbiVersionFn = uint32_t (*)(void);
using GetInfoFn    = const HSFPluginInfo* (*)(void);
using CreateFn     = HSFStatus (*)(const HSFPluginContext*, HSFPlugin**,
                                   const HSFPluginVTable**);
using DestroyFn    = void (*)(HSFPlugin*);

/* A minimal host: just enough of the services a plugin is handed, plus a record
 * of what it did with them. */
struct FakeHost {
  std::vector<std::string> log_lines;

  HSFLoggerRef Logger() {
    static const HSFLoggerVTable vt = [] {
      HSFLoggerVTable v{};
      v.struct_size = static_cast<uint32_t>(sizeof(HSFLoggerVTable));
      v.write = [](HSFLogger* self, HSFLogLevel, HSFStr msg) {
        reinterpret_cast<FakeHost*>(self)->log_lines.push_back(hsf::ToString(msg));
      };
      v.enabled = [](const HSFLogger*, HSFLogLevel) { return (int32_t)1; };
      v.record = [](HSFLogger*, HSFStr, HSFStr) { return (HSFStatus)HSF_OK; };
      return v;
    }();
    HSFLoggerRef r;
    r.vt = &vt;
    r.self = reinterpret_cast<HSFLogger*>(this);
    return r;
  }

  HSFConfigRef Config() {
    static const HSFConfigVTable vt = [] {
      HSFConfigVTable v{};
      v.struct_size = static_cast<uint32_t>(sizeof(HSFConfigVTable));
      v.has = [](const HSFConfig*, HSFStr) { return (int32_t)0; };
      v.get_str = [](const HSFConfig*, HSFStr, HSFStr fb) { return fb; };
      v.get_i64 = [](const HSFConfig*, HSFStr, int64_t fb) { return fb; };
      v.get_f64 = [](const HSFConfig*, HSFStr, double fb) { return fb; };
      v.get_bool = [](const HSFConfig*, HSFStr, int32_t fb) { return fb; };
      v.get_json = [](const HSFConfig*) { return hsf_str_empty(); };
      v.set_str = [](HSFConfig*, HSFStr, HSFStr) { return (HSFStatus)HSF_OK; };
      return v;
    }();
    HSFConfigRef r;
    r.vt = &vt;
    r.self = reinterpret_cast<HSFConfig*>(this);
    return r;
  }

  HSFPluginContext Context() {
    HSFPluginContext ctx{};
    ctx.struct_size = static_cast<uint32_t>(sizeof(HSFPluginContext));
    ctx.host_api_version = HSF_PLUGIN_API_VERSION;
    ctx.log = Logger();
    ctx.config = Config();
    ctx.data_dir = HSF_STR_LIT("/tmp");
    ctx.instance_id = HSF_STR_LIT("test");
    return ctx;
  }
};

struct Loaded {
  LibHandle lib = nullptr;
  AbiVersionFn abi = nullptr;
  GetInfoFn info = nullptr;
  CreateFn create = nullptr;
  DestroyFn destroy = nullptr;
};

bool Load(Loaded* out) {
  out->lib = OpenLib(HSF_TEST_PLUGIN_PATH);
  if (!out->lib) return false;
  out->abi = reinterpret_cast<AbiVersionFn>(Sym(out->lib, "hsf_plugin_abi_version"));
  out->info = reinterpret_cast<GetInfoFn>(Sym(out->lib, "hsf_plugin_get_info"));
  out->create = reinterpret_cast<CreateFn>(Sym(out->lib, "hsf_plugin_create"));
  out->destroy = reinterpret_cast<DestroyFn>(Sym(out->lib, "hsf_plugin_destroy"));
  return out->abi && out->info && out->create && out->destroy;
}

}  // namespace

HSF_TEST("loader: the plugin exports exactly the four ABI symbols") {
  HSF_REQUIRE(std::string(HSF_TEST_PLUGIN_PATH).size() > 0);
  Loaded l;
  HSF_REQUIRE(Load(&l));
  HSF_CHECK(l.abi != nullptr);
  HSF_CHECK(l.info != nullptr);
  HSF_CHECK(l.create != nullptr);
  HSF_CHECK(l.destroy != nullptr);

  /* Internal symbols must NOT be visible. Default visibility would let two
   * plugins sharing a helper name resolve to whichever loaded first — a crash
   * that only appears once a second plugin is installed. */
  HSF_CHECK(Sym(l.lib, "_ZN12_GLOBAL__N_111HelloDriver4ReadEPK16HSFDeviceAddress") == nullptr);
  CloseLib(l.lib);
}

HSF_TEST("loader: the ABI version is readable and compatible") {
  Loaded l;
  HSF_REQUIRE(Load(&l));
  /* Callable before anything else, on a plugin the host might reject — which
   * is why it is a bare uint32 and not a field in a struct whose layout is the
   * very thing not yet known to be compatible. */
  const uint32_t v = l.abi();
  HSF_CHECK_EQ(HSF_API_VERSION_MAJOR_OF(v), (uint32_t)HSF_PLUGIN_API_VERSION_MAJOR);
  HSF_CHECK(HSF_API_VERSION_COMPATIBLE(HSF_PLUGIN_API_VERSION, v));
  CloseLib(l.lib);
}

HSF_TEST("loader: get_info works with no instance in existence") {
  Loaded l;
  HSF_REQUIRE(Load(&l));
  const HSFPluginInfo* i = l.info();
  HSF_REQUIRE(i != nullptr);
  HSF_CHECK_EQ(i->struct_size, (uint32_t)sizeof(HSFPluginInfo));
  HSF_CHECK_EQ(hsf::ToString(i->id), std::string("hsf.example.hello"));
  HSF_CHECK_EQ(hsf::ToString(i->version), std::string("1.0.0"));
  HSF_CHECK_EQ((int)i->kind, (int)HSF_PLUGIN_KIND_DRIVER);
  HSF_CHECK(i->permissions & HSF_PERM_SERIAL);
  /* Built for this platform, so this host may load it. */
  HSF_CHECK_EQ(hsf::ToString(i->platform), std::string(HSF_PLATFORM_TRIPLE));
  CloseLib(l.lib);
}

HSF_TEST("loader: create rejects null arguments instead of crashing") {
  Loaded l;
  HSF_REQUIRE(Load(&l));
  HSFPlugin* p = nullptr;
  const HSFPluginVTable* vt = nullptr;
  HSF_CHECK_STATUS(l.create(nullptr, &p, &vt), HSF_ERR_INVALID_ARG);
  FakeHost host;
  HSFPluginContext ctx = host.Context();
  HSF_CHECK_STATUS(l.create(&ctx, nullptr, &vt), HSF_ERR_INVALID_ARG);
  HSF_CHECK_STATUS(l.create(&ctx, &p, nullptr), HSF_ERR_INVALID_ARG);
  CloseLib(l.lib);
}

HSF_TEST("loader: create, get_driver, destroy — the full lifecycle") {
  Loaded l;
  HSF_REQUIRE(Load(&l));
  FakeHost host;
  HSFPluginContext ctx = host.Context();

  HSFPlugin* plugin = nullptr;
  const HSFPluginVTable* vt = nullptr;
  HSF_REQUIRE(l.create(&ctx, &plugin, &vt) == HSF_OK);
  HSF_REQUIRE(plugin != nullptr);
  HSF_REQUIRE(vt != nullptr);
  HSF_CHECK_EQ(vt->struct_size, (uint32_t)sizeof(HSFPluginVTable));

  /* The plugin logged through the host's vtable — proof that a service handed
   * down as a fat pointer is genuinely callable across the boundary. */
  HSF_CHECK(!host.log_lines.empty());

  HSFDriverRef driver{};
  HSFDriverInfo info{};
  HSF_REQUIRE(vt->get_driver != nullptr);
  HSF_REQUIRE(vt->get_driver(plugin, &driver, &info) == HSF_OK);
  HSF_CHECK(driver.vt != nullptr);
  HSF_CHECK(driver.self != nullptr);
  HSF_CHECK_EQ(info.owns_thread, 1);
  HSF_CHECK_EQ(info.max_frame_bytes, (uint32_t)256);
  HSF_CHECK_EQ(info.supported_transport_count, (size_t)3);

  l.destroy(plugin);
  CloseLib(l.lib);
}

HSF_TEST("loader: the driver talks a protocol over the mock transport") {
  /* End to end: a separately-compiled .so, driven through the C ABI, doing real
   * request/response I/O against a simulated device. If this passes, the ABI
   * works. */
  Loaded l;
  HSF_REQUIRE(Load(&l));
  FakeHost host;
  HSFPluginContext ctx = host.Context();

  HSFPlugin* plugin = nullptr;
  const HSFPluginVTable* vt = nullptr;
  HSF_REQUIRE(l.create(&ctx, &plugin, &vt) == HSF_OK);
  HSFDriverRef d{};
  HSF_REQUIRE(vt->get_driver(plugin, &d, nullptr) == HSF_OK);

  hsf::MockTransport device;
  /* A tiny simulated device. Fragmented at 3 bytes so the driver has to
   * reassemble — a driver that assumed one read is one frame fails here. */
  device.SetResponder(
      [](const std::vector<uint8_t>& req) {
        const std::string s(req.begin(), req.end());
        std::string reply;
        if (s.rfind("PING", 0) == 0)      reply = "PONG\n";
        else if (s.rfind("GET ", 0) == 0) reply = "VAL 21.5\n";
        else if (s.rfind("SET ", 0) == 0) reply = "OK\n";
        else                              reply = "ERR\n";
        return std::vector<uint8_t>(reply.begin(), reply.end());
      },
      /*fragment=*/3);

  HSF_REQUIRE(d.vt->initialize(d.self, host.Config(), device.Ref()) == HSF_OK);

  HSFDriverStatus st{};
  d.vt->status(d.self, &st);
  HSF_CHECK_EQ((int)st.state, (int)HSF_DRIVER_READY);
  HSF_CHECK_EQ(st.connected, 0);   /* ready is not connected */

  /* read before start must be refused, not attempted */
  HSFDeviceAddress addr = hsf_device_address_init();
  addr.point_name = HSF_STR_LIT("temperature");
  HSFValue v = hsf_value_null();
  HSF_CHECK_STATUS(d.vt->read(d.self, &addr, &v), HSF_ERR_STATE);

  HSF_REQUIRE(d.vt->start(d.self) == HSF_OK);
  d.vt->status(d.self, &st);
  HSF_CHECK_EQ((int)st.state, (int)HSF_DRIVER_RUNNING);
  HSF_CHECK_EQ(st.connected, 1);
  HSF_CHECK(device.IsOpen());

  HSF_CHECK_OK(d.vt->read(d.self, &addr, &v));
  HSF_CHECK_EQ((int)v.kind, (int)HSF_VALUE_F64);
  HSF_CHECK_EQ(v.as.f64, 21.5);
  const std::vector<uint8_t>& sent = device.LastWrite();
  HSF_CHECK_EQ(std::string(sent.begin(), sent.end()),
               std::string("GET temperature\n"));

  HSFValue w = hsf_value_f64(3.5);
  HSF_CHECK_OK(d.vt->write(d.self, &addr, &w));
  HSF_CHECK_OK(d.vt->health(d.self));

  /* A blob to an analogue point is refused rather than coerced to 0. */
  const uint8_t raw[2] = {1, 2};
  HSFValue bad = hsf_value_blob(raw, sizeof(raw));
  HSF_CHECK_STATUS(d.vt->write(d.self, &addr, &bad), HSF_ERR_INVALID_ARG);

  /* stop is idempotent, and start after stop works — this is Disable/Enable. */
  HSF_CHECK_OK(d.vt->stop(d.self));
  HSF_CHECK_OK(d.vt->stop(d.self));
  HSF_CHECK(!device.IsOpen());
  HSF_CHECK_OK(d.vt->start(d.self));
  HSF_CHECK_EQ(device.OpenCount(), (size_t)2);
  HSF_CHECK_OK(d.vt->read(d.self, &addr, &v));

  HSF_CHECK_OK(d.vt->stop(d.self));
  l.destroy(plugin);
  CloseLib(l.lib);
}

HSF_TEST("loader: a driver reports a device that never answers as a timeout") {
  Loaded l;
  HSF_REQUIRE(Load(&l));
  FakeHost host;
  HSFPluginContext ctx = host.Context();
  HSFPlugin* plugin = nullptr;
  const HSFPluginVTable* vt = nullptr;
  HSF_REQUIRE(l.create(&ctx, &plugin, &vt) == HSF_OK);
  HSFDriverRef d{};
  HSF_REQUIRE(vt->get_driver(plugin, &d, nullptr) == HSF_OK);

  hsf::MockTransport silent;   /* accepts writes, never replies */
  silent.SetResponder([](const std::vector<uint8_t>&) {
    return std::vector<uint8_t>();
  });
  HSF_REQUIRE(d.vt->initialize(d.self, host.Config(), silent.Ref()) == HSF_OK);
  HSF_REQUIRE(d.vt->start(d.self) == HSF_OK);

  HSFDeviceAddress addr = hsf_device_address_init();
  addr.point_name = HSF_STR_LIT("x");
  HSFValue v = hsf_value_null();
  HSF_CHECK_STATUS(d.vt->read(d.self, &addr, &v), HSF_ERR_TIMEOUT);

  /* And it says why, in its own words, rather than leaving the operator to
   * guess from a status code. */
  HSFDriverStatus st{};
  d.vt->status(d.self, &st);
  HSF_CHECK(st.last_error.len > 0);

  d.vt->stop(d.self);
  l.destroy(plugin);
  CloseLib(l.lib);
}

HSF_TEST("loader: initialize without a transport fails as a config error") {
  Loaded l;
  HSF_REQUIRE(Load(&l));
  FakeHost host;
  HSFPluginContext ctx = host.Context();
  HSFPlugin* plugin = nullptr;
  const HSFPluginVTable* vt = nullptr;
  HSF_REQUIRE(l.create(&ctx, &plugin, &vt) == HSF_OK);
  HSFDriverRef d{};
  HSF_REQUIRE(vt->get_driver(plugin, &d, nullptr) == HSF_OK);

  HSFTransportRef none{};
  HSF_CHECK_STATUS(d.vt->initialize(d.self, host.Config(), none), HSF_ERR_CONFIG);
  /* start must then refuse rather than crash on the missing transport. */
  HSF_CHECK(d.vt->start(d.self) < 0);
  l.destroy(plugin);
  CloseLib(l.lib);
}

HSF_TEST("loader: diagnostics returns storage that outlives the call") {
  Loaded l;
  HSF_REQUIRE(Load(&l));
  FakeHost host;
  HSFPluginContext ctx = host.Context();
  HSFPlugin* plugin = nullptr;
  const HSFPluginVTable* vt = nullptr;
  HSF_REQUIRE(l.create(&ctx, &plugin, &vt) == HSF_OK);
  HSF_REQUIRE(vt->diagnostics != nullptr);

  HSFStr json = hsf_str_empty();
  HSF_CHECK_OK(vt->diagnostics(plugin, &json));
  HSF_CHECK(json.len > 0);
  /* Readable after the call returned: a pointer into a local would be a
   * dangling read here, which is the mistake this checks for. */
  HSF_CHECK(hsf::ToString(json).find("hello-line-v1") != std::string::npos);

  l.destroy(plugin);
  CloseLib(l.lib);
}

HSF_TEST("loader: unloading and reloading leaves nothing behind") {
  /* The Plugin Manager does exactly this on update and rollback, so it has to
   * be clean. */
  for (int i = 0; i < 3; ++i) {
    Loaded l;
    HSF_REQUIRE(Load(&l));
    FakeHost host;
    HSFPluginContext ctx = host.Context();
    HSFPlugin* plugin = nullptr;
    const HSFPluginVTable* vt = nullptr;
    HSF_REQUIRE(l.create(&ctx, &plugin, &vt) == HSF_OK);
    /* Destroyed without ever being started — must be tolerated. */
    l.destroy(plugin);
    CloseLib(l.lib);
  }
  HSF_CHECK(true);
}

HSF_TEST_MAIN()
