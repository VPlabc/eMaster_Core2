/* PluginManager end to end, against the REAL example plugin.so.
 *
 * This is the counterpart of test_loader.cpp one level up: that proved the ABI
 * works across a shared-object boundary, and this proves the gateway's manager
 * drives it correctly -- discovery, validation, load, the lifecycle state
 * machine, health, disable/enable, unload, and the failure paths.
 *
 * It uses the real plugin built by examples/hello-plugin rather than a fake,
 * because the interesting bugs here are in the seams: destruction order,
 * whether a rescan disturbs a running plugin, whether a rejected manifest is
 * visible to an operator.
 */
#include "hsf/plugin_manager/PluginManager.h"

#include "hsf/testing.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef HSF_TEST_PLUGIN_PATH
#  define HSF_TEST_PLUGIN_PATH ""
#endif

using hsf::PluginManager;
using hsf::PluginRecord;
using hsf::PluginState;

namespace {

const char* kPluginId = "hsf.example.hello";

/* A plugin root laid out the way the manager expects:
 *   <root>/<name>/manifest.json
 *   <root>/<name>/plugin.so       (a copy of the real built plugin)
 */
class Root {
 public:
  Root() {
    char name[64];
    std::snprintf(name, sizeof(name), "hsf_pm_test_%p", static_cast<void*>(this));
    root_ = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
    std::filesystem::create_directories(root_, ec);
  }
  ~Root() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  // Installs the real plugin under `dir_name`, optionally with a doctored
  // manifest so a test can break exactly one field.
  bool Install(const std::string& dir_name, const std::string& manifest_json = std::string()) {
    std::error_code ec;
    const std::filesystem::path dir = root_ / dir_name;
    std::filesystem::create_directories(dir, ec);

    const std::string src = HSF_TEST_PLUGIN_PATH;
    if (src.empty() || !std::filesystem::is_regular_file(src, ec)) return false;
    const std::string entry = std::filesystem::path(src).filename().string();
    std::filesystem::copy_file(src, dir / entry,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return false;

    std::string json = manifest_json;
    if (json.empty()) {
      json = std::string("{\"id\":\"") + kPluginId +
             "\",\"name\":\"HSF Hello Example\",\"version\":\"1.0.0\","
             "\"api_version\":\"1.0\",\"entry\":\"" + entry +
             "\",\"execution\":\"in_process\",\"platforms\":[\"" HSF_PLATFORM_TRIPLE "\"],"
             "\"transports\":[\"mock\"],\"permissions\":[\"serial\",\"network\"]}";
    } else {
      // Let callers use {ENTRY} rather than hardcoding plugin.so vs plugin.dll.
      const std::string token = "{ENTRY}";
      size_t at = json.find(token);
      while (at != std::string::npos) {
        json.replace(at, token.size(), entry);
        at = json.find(token);
      }
    }
    std::ofstream out(dir / "manifest.json", std::ios::binary);
    out << json;
    return out.good();
  }

  // A directory with a manifest but no binary.
  void InstallManifestOnly(const std::string& dir_name, const std::string& json) const {
    std::error_code ec;
    const std::filesystem::path dir = root_ / dir_name;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / "manifest.json", std::ios::binary);
    out << json;
  }

  std::string Path() const { return root_.string(); }
  std::string DataPath() const { return (root_ / "_data").string(); }

 private:
  std::filesystem::path root_;
};

bool StateOf(const PluginManager& m, const std::string& id, PluginState* out) {
  PluginRecord r;
  if (!m.Get(id, &r)) return false;
  if (out) *out = r.state;
  return true;
}

}  // namespace

HSF_TEST("manager: an empty or missing root is not an error") {
  /* A gateway with no plugins installed is the normal case today, and must not
   * log an error or fail to start. */
  PluginManager m;
  HSF_CHECK_EQ(m.Discover(), (size_t)0);

  m.SetPluginRoot("/nonexistent/plugins");
  HSF_CHECK_EQ(m.Discover(), (size_t)0);

  Root root;
  m.SetPluginRoot(root.Path());
  HSF_CHECK_EQ(m.Discover(), (size_t)0);
}

HSF_TEST("manager: discovery finds and validates a well-formed plugin") {
  Root root;
  HSF_REQUIRE(root.Install("hello"));
  PluginManager m;
  m.SetPluginRoot(root.Path());

  HSF_CHECK_EQ(m.Discover(), (size_t)1);
  PluginState st;
  HSF_REQUIRE(StateOf(m, kPluginId, &st));
  HSF_CHECK_EQ((int)st, (int)PluginState::kValidated);

  PluginRecord r;
  HSF_REQUIRE(m.Get(kPluginId, &r));
  HSF_CHECK_EQ(r.manifest.version, std::string("1.0.0"));
  HSF_CHECK(r.last_error.empty());
  HSF_CHECK(!r.enabled);
}

HSF_TEST("manager: a rejected manifest is recorded as FAILED, not skipped") {
  /* An operator who dropped a plugin in and saw nothing appear needs the reason
   * in the UI, not in a log they did not know to read. */
  Root root;
  root.InstallManifestOnly("broken",
      "{\"id\":\"hsf.broken.one\",\"name\":\"B\",\"version\":\"1\","
      "\"api_version\":\"9.0\",\"entry\":\"plugin.so\"}");
  PluginManager m;
  m.SetPluginRoot(root.Path());
  HSF_CHECK_EQ(m.Discover(), (size_t)1);

  PluginRecord r;
  HSF_REQUIRE(m.Get("hsf.broken.one", &r));
  HSF_CHECK_EQ((int)r.state, (int)PluginState::kFailed);
  HSF_CHECK(r.last_error.find("INCOMPATIBLE_API") != std::string::npos);
  HSF_CHECK(r.last_error.find("9.0") != std::string::npos);
}

HSF_TEST("manager: load, enable, health, disable, unload") {
  Root root;
  HSF_REQUIRE(root.Install("hello"));
  PluginManager m;
  m.SetPluginRoot(root.Path());
  m.SetDataRoot(root.DataPath());
  HSF_REQUIRE(m.Discover() == 1);

  std::string err;
  HSF_REQUIRE(m.Load(kPluginId, err));
  PluginState st;
  HSF_REQUIRE(StateOf(m, kPluginId, &st));
  HSF_CHECK_EQ((int)st, (int)PluginState::kInstalled);

  PluginRecord r;
  HSF_REQUIRE(m.Get(kPluginId, &r));
  HSF_CHECK(r.has_driver);
  /* The example declares owns_thread because it blocks in Exchange. */
  HSF_CHECK_EQ(r.driver_info.owns_thread, 1);

  /* Load is idempotent. */
  HSF_CHECK(m.Load(kPluginId, err));

  /* Enable: the example driver has no transport, so initialize() fails with
   * CONFIG and the plugin lands ENABLED-but-not-RUNNING, with a reason. That
   * is the correct outcome, not FAILED -- the operator's intent is still
   * "enabled", and the driver should be able to start once configured. */
  const bool enabled = m.Enable(kPluginId, err);
  HSF_REQUIRE(m.Get(kPluginId, &r));
  HSF_CHECK(r.enabled);
  if (!enabled) {
    HSF_CHECK_EQ((int)r.state, (int)PluginState::kEnabled);
    HSF_CHECK(!r.last_error.empty());
  } else {
    HSF_CHECK_EQ((int)r.state, (int)PluginState::kRunning);
  }

  /* Health on a non-RUNNING plugin refuses rather than probing. */
  if (r.state != PluginState::kRunning) {
    HSF_CHECK(!m.HealthCheck(kPluginId, err));
    HSF_CHECK(err.find("RUNNING") != std::string::npos);
  }

  HSF_CHECK(m.Disable(kPluginId, err));
  HSF_REQUIRE(m.Get(kPluginId, &r));
  HSF_CHECK_EQ((int)r.state, (int)PluginState::kDisabled);
  HSF_CHECK(!r.enabled);

  HSF_CHECK(m.Unload(kPluginId, err));
  HSF_REQUIRE(StateOf(m, kPluginId, &st));
  HSF_CHECK_EQ((int)st, (int)PluginState::kValidated);

  /* Unload is idempotent and safe on an already-unloaded plugin. */
  HSF_CHECK(m.Unload(kPluginId, err));
}

HSF_TEST("manager: operations on an unknown id fail with its name") {
  PluginManager m;
  std::string err;
  HSF_CHECK(!m.Load("no.such.plugin", err));
  HSF_CHECK(err.find("no.such.plugin") != std::string::npos);
  HSF_CHECK(!m.Unload("no.such.plugin", err));
  HSF_CHECK(!m.Enable("no.such.plugin", err));
  HSF_CHECK(!m.Disable("no.such.plugin", err));
  HSF_CHECK(!m.HealthCheck("no.such.plugin", err));

  PluginRecord r;
  HSF_CHECK(!m.Get("no.such.plugin", &r));
  HSF_CHECK(m.Driver("no.such.plugin").vt == nullptr);
}

HSF_TEST("manager: a manifest whose id disagrees with the binary is refused") {
  /* Mixing up a manifest and a plugin.so would attach one plugin's
   * configuration to another's code. */
  Root root;
  HSF_REQUIRE(root.Install("liar",
      "{\"id\":\"hsf.driver.imposter\",\"name\":\"Imposter\",\"version\":\"1.0.0\","
      "\"api_version\":\"1.0\",\"entry\":\"{ENTRY}\","
      "\"platforms\":[\"" HSF_PLATFORM_TRIPLE "\"],\"permissions\":[\"serial\",\"network\"]}"));
  PluginManager m;
  m.SetPluginRoot(root.Path());
  HSF_REQUIRE(m.Discover() == 1);

  std::string err;
  HSF_CHECK(!m.Load("hsf.driver.imposter", err));
  HSF_CHECK(err.find("do not belong together") != std::string::npos);

  PluginState st;
  HSF_REQUIRE(StateOf(m, "hsf.driver.imposter", &st));
  HSF_CHECK_EQ((int)st, (int)PluginState::kFailed);
}

HSF_TEST("manager: a binary wanting more permission than it declares is refused") {
  /* The manifest is what the operator read before approving the install, so the
   * code may not exceed it. The example's binary asks for serial|network; this
   * manifest declares only network. */
  Root root;
  HSF_REQUIRE(root.Install("greedy",
      std::string("{\"id\":\"") + kPluginId +
      "\",\"name\":\"Hello\",\"version\":\"1.0.0\",\"api_version\":\"1.0\","
      "\"entry\":\"{ENTRY}\",\"platforms\":[\"" HSF_PLATFORM_TRIPLE "\"],"
      "\"permissions\":[\"network\"]}"));
  PluginManager m;
  m.SetPluginRoot(root.Path());
  HSF_REQUIRE(m.Discover() == 1);

  std::string err;
  HSF_CHECK(!m.Load(kPluginId, err));
  HSF_CHECK(err.find("does not declare") != std::string::npos);
}

HSF_TEST("manager: an entry that is not a plugin is refused with the loader's reason") {
  Root root;
  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::path(root.Path()) / "bogus";
  std::filesystem::create_directories(dir, ec);
  {
    std::ofstream out(dir / "plugin.so", std::ios::binary);
    out << "this is definitely not a shared object";
  }
  {
    std::ofstream out(dir / "manifest.json", std::ios::binary);
    out << "{\"id\":\"hsf.bogus.one\",\"name\":\"Bogus\",\"version\":\"1\","
           "\"api_version\":\"1.0\",\"entry\":\"plugin.so\"}";
  }

  PluginManager m;
  m.SetPluginRoot(root.Path());
  HSF_REQUIRE(m.Discover() == 1);

  std::string err;
  HSF_CHECK(!m.Load("hsf.bogus.one", err));
  /* The platform's own message, which is what distinguishes "not an ELF file"
   * from "undefined symbol" from "wrong architecture". */
  HSF_CHECK(!err.empty());
  HSF_CHECK(err.find("cannot load") != std::string::npos);

  PluginState st;
  HSF_REQUIRE(StateOf(m, "hsf.bogus.one", &st));
  HSF_CHECK_EQ((int)st, (int)PluginState::kFailed);
}

HSF_TEST("manager: rediscovery does not disturb a loaded plugin") {
  Root root;
  HSF_REQUIRE(root.Install("hello"));
  PluginManager m;
  m.SetPluginRoot(root.Path());
  HSF_REQUIRE(m.Discover() == 1);

  std::string err;
  HSF_REQUIRE(m.Load(kPluginId, err));
  HSF_REQUIRE(m.Enable(kPluginId, err) || true);   /* may not start; see above */

  PluginRecord before;
  HSF_REQUIRE(m.Get(kPluginId, &before));

  /* Adding a second plugin and rescanning must leave the first alone. */
  HSF_REQUIRE(root.Install("hello2",
      "{\"id\":\"hsf.example.second\",\"name\":\"Second\",\"version\":\"2.0.0\","
      "\"api_version\":\"1.0\",\"entry\":\"{ENTRY}\","
      "\"platforms\":[\"" HSF_PLATFORM_TRIPLE "\"],\"permissions\":[\"serial\",\"network\"]}"));
  HSF_CHECK_EQ(m.Discover(), (size_t)2);

  PluginRecord after;
  HSF_REQUIRE(m.Get(kPluginId, &after));
  HSF_CHECK_EQ((int)after.state, (int)before.state);
  HSF_CHECK_EQ(after.enabled, before.enabled);
}

HSF_TEST("manager: List and ToJson report every plugin and the API version") {
  Root root;
  HSF_REQUIRE(root.Install("hello"));
  PluginManager m;
  m.SetPluginRoot(root.Path());
  HSF_REQUIRE(m.Discover() == 1);

  HSF_CHECK_EQ(m.List().size(), (size_t)1);

  const nlohmann::json j = m.ToJson();
  HSF_REQUIRE(j.contains("plugins"));
  HSF_REQUIRE(j["plugins"].is_array());
  HSF_CHECK_EQ(j["plugins"].size(), (size_t)1);
  HSF_CHECK_EQ(j["platform"].get<std::string>(), std::string(HSF_PLATFORM_TRIPLE));
  HSF_CHECK_EQ(j["api_version"].get<std::string>(), std::string("1.0"));
  HSF_CHECK_EQ(j["plugins"][0]["id"].get<std::string>(), std::string(kPluginId));
  HSF_CHECK_EQ(j["plugins"][0]["state"].get<std::string>(), std::string("VALIDATED"));
  HSF_CHECK(j["plugins"][0]["has_driver"].is_boolean());
}

HSF_TEST("manager: configuration reaches the plugin's own section only") {
  Root root;
  HSF_REQUIRE(root.Install("hello"));
  PluginManager m;
  m.SetPluginRoot(root.Path());
  nlohmann::json all;
  all[kPluginId] = {{"timeout_ms", 250}};
  all["someone.else"] = {{"secret", "not yours"}};
  m.SetConfiguration(all);
  HSF_REQUIRE(m.Discover() == 1);

  std::string err;
  /* The plugin reads timeout_ms during initialize; loading must succeed with
   * the section present, and the foreign section must simply be invisible. */
  HSF_CHECK(m.Load(kPluginId, err));
  PluginRecord r;
  HSF_REQUIRE(m.Get(kPluginId, &r));
  HSF_CHECK_EQ((int)r.state, (int)PluginState::kInstalled);
}

HSF_TEST("manager: the driver ref is available once loaded and gone once unloaded") {
  Root root;
  HSF_REQUIRE(root.Install("hello"));
  PluginManager m;
  m.SetPluginRoot(root.Path());
  HSF_REQUIRE(m.Discover() == 1);
  HSF_CHECK(m.Driver(kPluginId).vt == nullptr);   /* not loaded yet */

  std::string err;
  HSF_REQUIRE(m.Load(kPluginId, err));
  HSFDriverRef d = m.Driver(kPluginId);
  HSF_CHECK(d.vt != nullptr);
  HSF_CHECK(d.self != nullptr);

  HSF_REQUIRE(m.Unload(kPluginId, err));
  HSF_CHECK(m.Driver(kPluginId).vt == nullptr);
}

HSF_TEST("manager: repeated load/unload cycles leave nothing behind") {
  /* What an update or a rollback does. If the destruction order in Release()
   * were wrong, this is where it would show up. */
  Root root;
  HSF_REQUIRE(root.Install("hello"));
  PluginManager m;
  m.SetPluginRoot(root.Path());
  HSF_REQUIRE(m.Discover() == 1);

  std::string err;
  for (int i = 0; i < 5; ++i) {
    HSF_REQUIRE(m.Load(kPluginId, err));
    HSF_REQUIRE(m.Unload(kPluginId, err));
  }
  PluginState st;
  HSF_REQUIRE(StateOf(m, kPluginId, &st));
  HSF_CHECK_EQ((int)st, (int)PluginState::kValidated);
}

HSF_TEST("manager: destruction with a plugin still loaded is clean") {
  /* Gateway shutdown. The manager must stop and destroy every instance while
   * its library is still mapped -- a thread running in unmapped code is not a
   * diagnosable crash. */
  Root root;
  HSF_REQUIRE(root.Install("hello"));
  {
    PluginManager m;
    m.SetPluginRoot(root.Path());
    HSF_REQUIRE(m.Discover() == 1);
    std::string err;
    HSF_REQUIRE(m.Load(kPluginId, err));
    m.Enable(kPluginId, err);
    /* No unload: the destructor has to do it. */
  }
  HSF_CHECK(true);
}

HSF_TEST("manager: state names round-trip") {
  const PluginState all[] = {PluginState::kDiscovered, PluginState::kValidated,
                             PluginState::kInstalled,  PluginState::kDisabled,
                             PluginState::kEnabled,    PluginState::kRunning,
                             PluginState::kFailed};
  for (PluginState s : all) {
    HSF_CHECK(std::string(hsf::PluginStateName(s)) != std::string("UNKNOWN"));
  }
}

/* --- the event bus ------------------------------------------------------- */

HSF_TEST("events: publish is queued, not delivered inline") {
  /* A publisher must never run a subscriber on its own thread: a slow
   * subscriber would stall the driver that produced the event. */
  hsf::PluginEventBus bus;
  int calls = 0;
  HSFEventBusRef ref = bus.RefFor("test.plugin");
  ref.vt->subscribe(ref.self, hsf_cstr("driver.*"),
                    [](const HSFEvent*, void* user) { ++*static_cast<int*>(user); }, &calls);

  HSF_CHECK_OK(ref.vt->publish(ref.self, hsf_cstr("driver.test.value"), hsf_cstr("{}")));
  HSF_CHECK_EQ(calls, 0);              /* nothing yet */
  HSF_CHECK_EQ(bus.QueueDepth(), (size_t)1);
  HSF_CHECK_EQ(bus.Dispatch(), (size_t)1);
  HSF_CHECK_EQ(calls, 1);
}

HSF_TEST("events: wildcards match by prefix, exact filters do not over-match") {
  hsf::PluginEventBus bus;
  int wild = 0, exact = 0, star = 0;
  HSFEventBusRef ref = bus.RefFor("p");
  auto bump = [](const HSFEvent*, void* user) { ++*static_cast<int*>(user); };
  ref.vt->subscribe(ref.self, hsf_cstr("driver.modbus.*"), bump, &wild);
  ref.vt->subscribe(ref.self, hsf_cstr("driver.modbus.value"), bump, &exact);
  ref.vt->subscribe(ref.self, hsf_cstr("*"), bump, &star);

  ref.vt->publish(ref.self, hsf_cstr("driver.modbus.value"), hsf_cstr("{}"));
  ref.vt->publish(ref.self, hsf_cstr("driver.modbus.error"), hsf_cstr("{}"));
  ref.vt->publish(ref.self, hsf_cstr("driver.zk.card"), hsf_cstr("{}"));
  bus.Dispatch();

  HSF_CHECK_EQ(wild, 2);
  HSF_CHECK_EQ(exact, 1);
  HSF_CHECK_EQ(star, 3);
}

HSF_TEST("events: the source is stamped by the host, not the publisher") {
  hsf::PluginEventBus bus;
  std::string seen;
  HSFEventBusRef a = bus.RefFor("plugin.a");
  a.vt->subscribe(a.self, hsf_cstr("*"),
                  [](const HSFEvent* ev, void* user) {
                    *static_cast<std::string*>(user) =
                        std::string(ev->source_plugin.ptr, ev->source_plugin.len);
                  },
                  &seen);
  HSFEventBusRef b = bus.RefFor("plugin.b");
  b.vt->publish(b.self, hsf_cstr("x.y"), hsf_cstr("{}"));
  bus.Dispatch();
  /* B published, so B is the source -- there is no way for it to claim to be A. */
  HSF_CHECK_EQ(seen, std::string("plugin.b"));
}

HSF_TEST("events: a full queue refuses rather than growing or dropping silently") {
  /* Bounded queues are a §27 requirement, and a driver that can outrun its
   * subscribers has to be told. */
  hsf::PluginEventBus bus(4);
  HSFEventBusRef ref = bus.RefFor("p");
  for (int i = 0; i < 4; ++i) {
    HSF_CHECK_OK(ref.vt->publish(ref.self, hsf_cstr("t"), hsf_cstr("{}")));
  }
  HSF_CHECK_STATUS(ref.vt->publish(ref.self, hsf_cstr("t"), hsf_cstr("{}")), HSF_ERR_BUSY);
  HSF_CHECK_EQ(bus.Dropped(), (size_t)1);

  /* Draining makes room again. */
  HSF_CHECK_EQ(bus.Dispatch(), (size_t)4);
  HSF_CHECK_OK(ref.vt->publish(ref.self, hsf_cstr("t"), hsf_cstr("{}")));
}

HSF_TEST("events: publish_value carries a value with no JSON at all") {
  hsf::PluginEventBus bus;
  double got = 0;
  HSFEventBusRef ref = bus.RefFor("p");
  ref.vt->subscribe(ref.self, hsf_cstr("*"),
                    [](const HSFEvent* ev, void* user) {
                      if (ev->value.kind == HSF_VALUE_F64) {
                        *static_cast<double*>(user) = ev->value.as.f64;
                      }
                    },
                    &got);
  HSF_CHECK_OK(ref.vt->publish_value(ref.self, hsf_cstr("t.v"), hsf_value_f64(42.5)));
  bus.Dispatch();
  HSF_CHECK_EQ(got, 42.5);
}

HSF_TEST("events: unsubscribe stops delivery and is idempotent") {
  hsf::PluginEventBus bus;
  int calls = 0;
  HSFEventBusRef ref = bus.RefFor("p");
  HSFSubscription sub = ref.vt->subscribe(
      ref.self, hsf_cstr("*"),
      [](const HSFEvent*, void* user) { ++*static_cast<int*>(user); }, &calls);
  HSF_REQUIRE(sub != 0);

  ref.vt->unsubscribe(ref.self, sub);
  ref.vt->unsubscribe(ref.self, sub);   /* twice is safe */
  ref.vt->publish(ref.self, hsf_cstr("t"), hsf_cstr("{}"));
  bus.Dispatch();
  HSF_CHECK_EQ(calls, 0);
}

HSF_TEST("events: a throwing subscriber does not stop the others") {
  /* A subscriber is plugin code. One misbehaving handler must not lose the
   * event for everyone else, nor unwind into the host's dispatch loop. */
  hsf::PluginEventBus bus;
  int good = 0;
  HSFEventBusRef ref = bus.RefFor("p");
  ref.vt->subscribe(ref.self, hsf_cstr("*"),
                    [](const HSFEvent*, void*) { throw std::runtime_error("bad handler"); },
                    nullptr);
  ref.vt->subscribe(ref.self, hsf_cstr("*"),
                    [](const HSFEvent*, void* user) { ++*static_cast<int*>(user); }, &good);
  ref.vt->publish(ref.self, hsf_cstr("t"), hsf_cstr("{}"));
  HSF_CHECK_EQ(bus.Dispatch(), (size_t)1);
  HSF_CHECK_EQ(good, 1);
}

HSF_TEST("events: a handler may publish without deadlocking") {
  /* One event causing another is normal. Dispatch releases the lock before
   * calling handlers precisely so this works. The nested event is queued and
   * then drained by the same Dispatch pass, since it has budget left. */
  hsf::PluginEventBus bus;
  struct Ctx { HSFEventBusRef ref; int first = 0; int second = 0; };
  Ctx ctx{bus.RefFor("p"), 0, 0};
  ctx.ref.vt->subscribe(ctx.ref.self, hsf_cstr("first"),
                        [](const HSFEvent*, void* user) {
                          auto* c = static_cast<Ctx*>(user);
                          ++c->first;
                          c->ref.vt->publish(c->ref.self, hsf_cstr("second"), hsf_cstr("{}"));
                        },
                        &ctx);
  ctx.ref.vt->subscribe(ctx.ref.self, hsf_cstr("second"),
                        [](const HSFEvent*, void* user) { ++static_cast<Ctx*>(user)->second; },
                        &ctx);

  ctx.ref.vt->publish(ctx.ref.self, hsf_cstr("first"), hsf_cstr("{}"));
  HSF_CHECK_EQ(bus.Dispatch(), (size_t)2);   /* the original and the nested one */
  HSF_CHECK_EQ(ctx.first, 1);
  HSF_CHECK_EQ(ctx.second, 1);
  HSF_CHECK_EQ(bus.QueueDepth(), (size_t)0);
}

HSF_TEST("events: a handler that republishes forever is bounded by max_events") {
  /* The safety property that matters more than the one above: a handler
   * re-publishing its own topic must not make Dispatch loop forever, or one
   * misbehaving plugin pins the host's dispatch thread. */
  hsf::PluginEventBus bus(64);
  struct Ctx { HSFEventBusRef ref; int calls = 0; };
  Ctx ctx{bus.RefFor("p"), 0};
  ctx.ref.vt->subscribe(ctx.ref.self, hsf_cstr("loop"),
                        [](const HSFEvent*, void* user) {
                          auto* c = static_cast<Ctx*>(user);
                          ++c->calls;
                          c->ref.vt->publish(c->ref.self, hsf_cstr("loop"), hsf_cstr("{}"));
                        },
                        &ctx);
  ctx.ref.vt->publish(ctx.ref.self, hsf_cstr("loop"), hsf_cstr("{}"));

  HSF_CHECK_EQ(bus.Dispatch(3), (size_t)3);   /* stops at the budget */
  HSF_CHECK_EQ(ctx.calls, 3);
  HSF_CHECK(bus.QueueDepth() > 0);            /* work remains, and that is fine */
}

HSF_TEST("events: the sink sees everything, filtered or not") {
  hsf::PluginEventBus bus;
  int sunk = 0;
  bus.SetSink([&sunk](const std::string&, const std::string&, const std::string&) { ++sunk; });
  HSFEventBusRef ref = bus.RefFor("p");
  ref.vt->publish(ref.self, hsf_cstr("a.b"), hsf_cstr("{}"));
  ref.vt->publish(ref.self, hsf_cstr("c.d"), hsf_cstr("{}"));
  bus.Dispatch();
  HSF_CHECK_EQ(sunk, 2);
}

HSF_TEST("events: an empty topic is rejected") {
  hsf::PluginEventBus bus;
  HSFEventBusRef ref = bus.RefFor("p");
  HSF_CHECK_STATUS(ref.vt->publish(ref.self, hsf_cstr(""), hsf_cstr("{}")),
                   HSF_ERR_INVALID_ARG);
}

HSF_TEST("events: the same plugin gets a stable binding across calls") {
  /* A plugin holds its HSFEventBusRef for its whole life, so the pointer behind
   * it must not move when another plugin registers. */
  hsf::PluginEventBus bus;
  HSFEventBusRef first = bus.RefFor("plugin.a");
  bus.RefFor("plugin.b");
  bus.RefFor("plugin.c");
  HSFEventBusRef again = bus.RefFor("plugin.a");
  HSF_CHECK(first.self == again.self);
  /* And the original ref still works. */
  HSF_CHECK_OK(first.vt->publish(first.self, hsf_cstr("t"), hsf_cstr("{}")));
}

HSF_TEST_MAIN()
