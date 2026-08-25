/* Tests for the example driver — the pattern to copy for your own.
 *
 * Two layers, and the split is the point:
 *
 *   1. PROTOCOL tests call BuildRequest / FrameLength / ParseReply directly.
 *      No transport, no timing, no device. These are where framing and
 *      encoding bugs get caught, and they run in microseconds.
 *
 *   2. BEHAVIOUR tests drive the full lifecycle over the mock transport, with
 *      the mock configured to be hostile — fragmented replies, silence,
 *      truncated writes, mid-stream errors. These catch the bugs that only
 *      appear when the protocol code meets real I/O.
 *
 * Write layer 1 first. It is where most of your bugs are, and it does not need
 * the SDK at all.
 */
#include "example_driver.h"

#include "hsf/mock_transport.hpp"
#include "hsf/testing.hpp"

#include <string>
#include <vector>

using example::Bytes;
using example::ExampleDriver;

static std::string S(const Bytes& b) { return std::string(b.begin(), b.end()); }
static Bytes B(const std::string& s) { return Bytes(s.begin(), s.end()); }

/* ---------------------- layer 1: protocol, no I/O ------------------------ */

HSF_TEST("protocol: a read request is framed correctly") {
  ExampleDriver d;
  HSF_CHECK_EQ(S(d.BuildRequest("temperature")), std::string("GET temperature\n"));
}

HSF_TEST("protocol: a write request formats the number without trailing zeros") {
  ExampleDriver d;
  HSF_CHECK_EQ(S(d.BuildWriteRequest("setpoint", 21.5)),
               std::string("SET setpoint 21.5\n"));
  HSF_CHECK_EQ(S(d.BuildWriteRequest("setpoint", 3.0)),
               std::string("SET setpoint 3\n"));
}

HSF_TEST("protocol: FrameLength needs the terminator before it commits") {
  ExampleDriver d;
  HSF_CHECK_EQ(d.FrameLength(B("")), (size_t)0);
  HSF_CHECK_EQ(d.FrameLength(B("VAL 1")), (size_t)0);      /* incomplete */
  HSF_CHECK_EQ(d.FrameLength(B("VAL 1\n")), (size_t)6);
  /* Two frames coalesced: reports only the FIRST, so the caller keeps the
   * remainder instead of discarding it. Getting this wrong silently drops
   * every second reply. */
  HSF_CHECK_EQ(d.FrameLength(B("VAL 1\nVAL 2\n")), (size_t)6);
}

HSF_TEST("protocol: ParseReply reads a value") {
  ExampleDriver d;
  HSFValue v = hsf_value_null();
  std::string err;
  HSF_CHECK_OK(d.ParseReply(B("VAL 21.5\n"), &v, &err));
  HSF_CHECK_EQ((int)v.kind, (int)HSF_VALUE_F64);
  HSF_CHECK_EQ(v.as.f64, 21.5);
  HSF_CHECK(err.empty());
}

HSF_TEST("protocol: ParseReply tolerates CRLF as well as LF") {
  ExampleDriver d;
  HSFValue v = hsf_value_null();
  std::string err;
  HSF_CHECK_OK(d.ParseReply(B("VAL 7\r\n"), &v, &err));
  HSF_CHECK_EQ(v.as.f64, 7.0);
}

HSF_TEST("protocol: a non-numeric VAL is a protocol error, not a zero") {
  /* Returning 0.0 here would put a plausible wrong reading into the historian,
   * which is worse than an error nobody can miss. */
  ExampleDriver d;
  HSFValue v = hsf_value_null();
  std::string err;
  HSF_CHECK_STATUS(d.ParseReply(B("VAL abc\n"), &v, &err), HSF_ERR_PROTOCOL);
  HSF_CHECK(!err.empty());
}

HSF_TEST("protocol: a device error reply carries the device's own words") {
  ExampleDriver d;
  HSFValue v = hsf_value_null();
  std::string err;
  HSF_CHECK_STATUS(d.ParseReply(B("ERR no such point\n"), &v, &err),
                   HSF_ERR_PROTOCOL);
  HSF_CHECK(err.find("no such point") != std::string::npos);
}

HSF_TEST("protocol: OK and PONG are recognised") {
  ExampleDriver d;
  HSFValue v = hsf_value_null();
  std::string err;
  HSF_CHECK_OK(d.ParseReply(B("OK\n"), &v, &err));
  HSF_CHECK_OK(d.ParseReply(B("PONG\n"), &v, &err));
}

/* ------------------- layer 2: behaviour over the mock -------------------- */

namespace {

/* A simulated device, the whole thing in one lambda. */
hsf::MockTransport::Responder Device() {
  return [](const Bytes& req) {
    const std::string s = S(req);
    std::string reply;
    if (s.rfind("PING", 0) == 0)      reply = "PONG\n";
    else if (s.rfind("GET ", 0) == 0) reply = "VAL 21.5\n";
    else if (s.rfind("SET ", 0) == 0) reply = "OK\n";
    else                              reply = "ERR unknown\n";
    return B(reply);
  };
}

HSFConfigRef NoConfig() { return HSFConfigRef{}; }  /* every getter falls back */

HSFDeviceAddress Point(const char* name) {
  HSFDeviceAddress a = hsf_device_address_init();
  a.point_name = hsf_cstr(name);
  return a;
}

}  // namespace

HSF_TEST("driver: the lifecycle enforces its own order") {
  ExampleDriver d;
  hsf::MockTransport t;
  t.SetResponder(Device());

  HSFValue v = hsf_value_null();
  HSFDeviceAddress a = Point("temperature");
  /* Reading before initialize is refused, not attempted. */
  HSF_CHECK_STATUS(d.Read(&a, &v), HSF_ERR_STATE);

  HSF_REQUIRE(d.Initialize(NoConfig(), t.Ref()) == HSF_OK);
  HSF_CHECK_STATUS(d.Read(&a, &v), HSF_ERR_STATE);   /* ready, not running */

  HSF_REQUIRE(d.Start() == HSF_OK);
  HSF_CHECK_OK(d.Read(&a, &v));
  HSF_CHECK_EQ(v.as.f64, 21.5);
}

HSF_TEST("driver: initialize without a transport is a config error") {
  ExampleDriver d;
  HSF_CHECK_STATUS(d.Initialize(NoConfig(), HSFTransportRef{}), HSF_ERR_CONFIG);
  HSF_CHECK(d.Start() < 0);
}

HSF_TEST("driver: a reply split across reads is reassembled") {
  ExampleDriver d;
  hsf::MockTransport t;
  t.SetResponder(Device(), /*fragment=*/2);   /* "VAL 21.5\n" in 2-byte pieces */
  HSF_REQUIRE(d.Initialize(NoConfig(), t.Ref()) == HSF_OK);
  HSF_REQUIRE(d.Start() == HSF_OK);

  HSFValue v = hsf_value_null();
  HSFDeviceAddress a = Point("temperature");
  HSF_CHECK_OK(d.Read(&a, &v));
  HSF_CHECK_EQ(v.as.f64, 21.5);
}

HSF_TEST("driver: a silent device times out and reports unreachable") {
  ExampleDriver d;
  hsf::MockTransport t;
  t.SetResponder([](const Bytes&) { return Bytes(); });   /* never answers */
  d.SetTimeoutMs(5);
  d.SetRetries(1);
  HSF_REQUIRE(d.Initialize(NoConfig(), t.Ref()) == HSF_OK);
  d.SetTimeoutMs(5);        /* Initialize re-read it from the null config */
  d.SetRetries(1);
  HSF_REQUIRE(d.Start() == HSF_OK);

  HSFValue v = hsf_value_null();
  HSFDeviceAddress a = Point("x");
  HSF_CHECK_STATUS(d.Read(&a, &v), HSF_ERR_TIMEOUT);

  HSFDriverStatus st{};
  d.Status(&st);
  /* Still RUNNING — the driver is fine, the device is not. A dashboard has to
   * be able to say which. */
  HSF_CHECK_EQ((int)st.state, (int)HSF_DRIVER_RUNNING);
  HSF_CHECK_EQ(st.connected, 0);
  HSF_CHECK(st.last_error.len > 0);
  HSF_CHECK(st.reads_failed >= 2);   /* initial attempt plus one retry */
}

HSF_TEST("driver: retries flush stale input before trying again") {
  /* The RS485 case: a leftover fragment is sitting in the buffer. Without a
   * flush it gets parsed as the head of the next reply and every subsequent
   * exchange is off by one frame. */
  ExampleDriver d;
  hsf::MockTransport t;
  t.PushReadable("GARBAGE");            /* no terminator: unparseable */
  t.SetResponder(Device());
  HSF_REQUIRE(d.Initialize(NoConfig(), t.Ref()) == HSF_OK);
  d.SetTimeoutMs(5);
  d.SetRetries(2);
  HSF_REQUIRE(d.Start() == HSF_OK);

  HSFValue v = hsf_value_null();
  HSFDeviceAddress a = Point("temperature");
  /* First attempt is poisoned by the stale bytes; the retry flushes and
   * succeeds. */
  HSF_CHECK_OK(d.Read(&a, &v));
  HSF_CHECK_EQ(v.as.f64, 21.5);
}

HSF_TEST("driver: a truncated write is completed, not shipped short") {
  ExampleDriver d;
  hsf::MockTransport t;
  t.SetResponder(Device());
  t.LimitNextWrite(4);
  HSF_REQUIRE(d.Initialize(NoConfig(), t.Ref()) == HSF_OK);
  HSF_REQUIRE(d.Start() == HSF_OK);

  HSFValue v = hsf_value_null();
  HSFDeviceAddress a = Point("temperature");
  HSF_CHECK_OK(d.Read(&a, &v));
  HSF_REQUIRE(t.WriteCount() == 1);
  HSF_CHECK_EQ(S(t.LastWrite()), std::string("GET temperature\n"));
}

HSF_TEST("driver: writing a blob to a numeric point is refused") {
  ExampleDriver d;
  hsf::MockTransport t;
  t.SetResponder(Device());
  HSF_REQUIRE(d.Initialize(NoConfig(), t.Ref()) == HSF_OK);
  HSF_REQUIRE(d.Start() == HSF_OK);

  const uint8_t raw[3] = {1, 2, 3};
  HSFValue bad = hsf_value_blob(raw, sizeof(raw));
  HSFDeviceAddress a = Point("setpoint");
  HSF_CHECK_STATUS(d.Write(&a, &bad), HSF_ERR_INVALID_ARG);
  HSF_CHECK_EQ(t.WriteCount(), (size_t)0);   /* nothing reached the device */
}

HSF_TEST("driver: bool, int and float all write") {
  ExampleDriver d;
  hsf::MockTransport t;
  t.SetResponder(Device());
  HSF_REQUIRE(d.Initialize(NoConfig(), t.Ref()) == HSF_OK);
  HSF_REQUIRE(d.Start() == HSF_OK);
  HSFDeviceAddress a = Point("setpoint");

  HSFValue f = hsf_value_f64(1.25);
  HSFValue i = hsf_value_i64(42);
  HSFValue b = hsf_value_bool(1);
  HSF_CHECK_OK(d.Write(&a, &f));
  HSF_CHECK_OK(d.Write(&a, &i));
  HSF_CHECK_OK(d.Write(&a, &b));
  HSF_CHECK_EQ(t.WriteCount(), (size_t)3);
}

HSF_TEST("driver: stop is idempotent and start-after-stop works") {
  /* This is what the Enable/Disable buttons do, so it has to be safe. */
  ExampleDriver d;
  hsf::MockTransport t;
  t.SetResponder(Device());
  HSF_REQUIRE(d.Initialize(NoConfig(), t.Ref()) == HSF_OK);
  HSF_REQUIRE(d.Start() == HSF_OK);
  HSF_CHECK_OK(d.Stop());
  HSF_CHECK_OK(d.Stop());
  HSF_CHECK(!t.IsOpen());
  HSF_CHECK_OK(d.Start());
  HSF_CHECK_EQ(t.OpenCount(), (size_t)2);

  HSFValue v = hsf_value_null();
  HSFDeviceAddress a = Point("temperature");
  HSF_CHECK_OK(d.Read(&a, &v));
}

HSF_TEST("driver: health tracks reachability both ways") {
  ExampleDriver d;
  hsf::MockTransport t;
  t.SetResponder(Device());
  HSF_REQUIRE(d.Initialize(NoConfig(), t.Ref()) == HSF_OK);
  HSF_REQUIRE(d.Start() == HSF_OK);
  HSF_CHECK_OK(d.Health());

  HSFDriverStatus st{};
  d.Status(&st);
  HSF_CHECK_EQ(st.connected, 1);

  /* Device goes quiet: health must notice. */
  t.SetResponder([](const Bytes&) { return Bytes(); });
  d.SetTimeoutMs(5);
  d.SetRetries(0);
  HSF_CHECK(d.Health() < 0);
  d.Status(&st);
  HSF_CHECK_EQ(st.connected, 0);
}

HSF_TEST("driver: null arguments are rejected, not dereferenced") {
  ExampleDriver d;
  hsf::MockTransport t;
  t.SetResponder(Device());
  HSF_REQUIRE(d.Initialize(NoConfig(), t.Ref()) == HSF_OK);
  HSF_REQUIRE(d.Start() == HSF_OK);
  HSFValue v = hsf_value_null();
  HSFDeviceAddress a = Point("x");
  HSF_CHECK_STATUS(d.Read(nullptr, &v), HSF_ERR_INVALID_ARG);
  HSF_CHECK_STATUS(d.Read(&a, nullptr), HSF_ERR_INVALID_ARG);
  HSF_CHECK_STATUS(d.Write(nullptr, &v), HSF_ERR_INVALID_ARG);
  HSF_CHECK_STATUS(d.Write(&a, nullptr), HSF_ERR_INVALID_ARG);
}

HSF_TEST_MAIN()
