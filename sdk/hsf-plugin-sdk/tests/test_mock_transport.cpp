/* Tests for the mock transport and for BlockingIo, which is built on it.
 *
 * These matter more than they look. The mock is what every plugin author's
 * tests will run against, so a bug here becomes a bug in everyone's test
 * suite — and worse, a mock that is kinder than real hardware produces drivers
 * that pass their tests and fail on site. Most of what follows checks that the
 * mock is appropriately hostile.
 */
#include "hsf/mock_transport.hpp"
#include "hsf/testing.hpp"

#include <string>
#include <vector>

using hsf::MockTransport;
using Bytes = std::vector<uint8_t>;

static Bytes B(const std::string& s) { return Bytes(s.begin(), s.end()); }
static std::string S(const Bytes& b) { return std::string(b.begin(), b.end()); }

HSF_TEST("mock: open, close, and the double-open refusal") {
  MockTransport m;
  HSFTransportRef t = m.Ref();

  HSF_CHECK_EQ(t.vt->is_open(t.self), 0);
  HSF_CHECK_OK(t.vt->open(t.self));
  HSF_CHECK_EQ(t.vt->is_open(t.self), 1);
  /* Not merely tolerated — reported. A driver that opens twice has a lifecycle
   * bug, and hiding it makes it surface later as a leaked handle. */
  HSF_CHECK_STATUS(t.vt->open(t.self), HSF_ERR_ALREADY_OPEN);
  t.vt->close(t.self);
  HSF_CHECK_EQ(t.vt->is_open(t.self), 0);
  HSF_CHECK_EQ(m.OpenCount(), (size_t)1);
}

HSF_TEST("mock: I/O before open is refused, not silently buffered") {
  MockTransport m;
  HSFTransportRef t = m.Ref();
  char buf[8];
  size_t n = 0;
  HSF_CHECK_STATUS(t.vt->read(t.self, buf, sizeof(buf), &n), HSF_ERR_NOT_OPEN);
  HSF_CHECK_STATUS(t.vt->write(t.self, "x", 1, &n), HSF_ERR_NOT_OPEN);
}

HSF_TEST("mock: an empty inbox is AGAIN, a closed peer is OK with zero bytes") {
  /* THE distinction non-blocking I/O turns on. If these two were the same
   * value a driver could not tell "nothing yet" from "never again", and would
   * either spin forever or give up on a live link. */
  MockTransport m;
  HSFTransportRef t = m.Ref();
  HSF_REQUIRE(t.vt->open(t.self) == HSF_OK);

  char buf[8];
  size_t n = 123;
  HSF_CHECK_STATUS(t.vt->read(t.self, buf, sizeof(buf), &n), HSF_AGAIN);
  HSF_CHECK_EQ(n, (size_t)0);

  m.CloseRemote();
  n = 123;
  HSF_CHECK_STATUS(t.vt->read(t.self, buf, sizeof(buf), &n), HSF_OK);
  HSF_CHECK_EQ(n, (size_t)0);
}

HSF_TEST("mock: reads are capped by the caller's buffer, remainder is kept") {
  MockTransport m;
  m.PushReadable("HELLO WORLD");
  HSFTransportRef t = m.Ref();
  HSF_REQUIRE(t.vt->open(t.self) == HSF_OK);

  char buf[5];
  size_t n = 0;
  HSF_CHECK_OK(t.vt->read(t.self, buf, sizeof(buf), &n));
  HSF_CHECK_EQ(n, (size_t)5);
  HSF_CHECK_EQ(std::string(buf, n), std::string("HELLO"));

  char rest[64];
  HSF_CHECK_OK(t.vt->read(t.self, rest, sizeof(rest), &n));
  HSF_CHECK_EQ(std::string(rest, n), std::string(" WORLD"));
}

HSF_TEST("mock: stalled reads return AGAIN the requested number of times") {
  MockTransport m;
  m.PushReadable("data");
  m.StallReads(3);
  HSFTransportRef t = m.Ref();
  HSF_REQUIRE(t.vt->open(t.self) == HSF_OK);

  char buf[16];
  size_t n = 0;
  for (int i = 0; i < 3; ++i) {
    HSF_CHECK_STATUS(t.vt->read(t.self, buf, sizeof(buf), &n), HSF_AGAIN);
  }
  HSF_CHECK_OK(t.vt->read(t.self, buf, sizeof(buf), &n));
  HSF_CHECK_EQ(n, (size_t)4);
}

HSF_TEST("mock: a responder answers each written frame") {
  MockTransport m;
  m.SetResponder([](const Bytes& req) {
    return (S(req) == "PING\n") ? B("PONG\n") : B("ERR\n");
  });
  HSFTransportRef t = m.Ref();
  HSF_REQUIRE(t.vt->open(t.self) == HSF_OK);

  size_t n = 0;
  HSF_CHECK_OK(t.vt->write(t.self, "PING\n", 5, &n));
  HSF_CHECK_EQ(n, (size_t)5);
  HSF_CHECK_EQ(m.WriteCount(), (size_t)1);

  char buf[32];
  HSF_CHECK_OK(t.vt->read(t.self, buf, sizeof(buf), &n));
  HSF_CHECK_EQ(std::string(buf, n), std::string("PONG\n"));
}

HSF_TEST("mock: fragmenting forces the driver to reassemble") {
  MockTransport m;
  m.SetResponder([](const Bytes&) { return B("VAL 42.5\n"); }, /*fragment=*/2);
  HSFTransportRef t = m.Ref();
  HSF_REQUIRE(t.vt->open(t.self) == HSF_OK);

  size_t n = 0;
  HSF_REQUIRE(t.vt->write(t.self, "GET x\n", 6, &n) == HSF_OK);

  /* Nine bytes in two-byte chunks: no single read sees the whole reply, which
   * is exactly what a real serial link does and what a naive driver assumes
   * away. */
  std::string acc;
  char buf[64];
  for (int i = 0; i < 10 && acc.find('\n') == std::string::npos; ++i) {
    size_t got = 0;
    HSFStatus st = t.vt->read(t.self, buf, sizeof(buf), &got);
    if (st == HSF_AGAIN) continue;
    HSF_REQUIRE(st == HSF_OK);
    acc.append(buf, got);
    HSF_CHECK(got <= 2);
  }
  HSF_CHECK_EQ(acc, std::string("VAL 42.5\n"));
}

HSF_TEST("mock: a limited write is partial, and the frame completes on retry") {
  MockTransport m;
  m.SetResponder([](const Bytes& req) { return B("SAW:" + S(req)); });
  m.LimitNextWrite(3);
  HSFTransportRef t = m.Ref();
  HSF_REQUIRE(t.vt->open(t.self) == HSF_OK);

  size_t n = 0;
  HSF_CHECK_OK(t.vt->write(t.self, "ABCDEF", 6, &n));
  HSF_CHECK_EQ(n, (size_t)3);
  /* Nothing delivered yet: half a frame is not a frame, and the responder must
   * not see one. */
  HSF_CHECK_EQ(m.WriteCount(), (size_t)0);

  HSF_CHECK_OK(t.vt->write(t.self, "DEF", 3, &n));
  HSF_CHECK_EQ(n, (size_t)3);
  HSF_REQUIRE(m.WriteCount() == 1);
  HSF_CHECK_EQ(S(m.LastWrite()), std::string("ABCDEF"));
}

HSF_TEST("mock: wait reports readability and times out on an empty inbox") {
  MockTransport m;
  HSFTransportRef t = m.Ref();
  HSF_REQUIRE(t.vt->open(t.self) == HSF_OK);

  uint32_t ready = 0;
  /* Write-ready always, so asking for WRITE succeeds even with nothing queued. */
  HSF_CHECK_OK(t.vt->wait(t.self, HSF_IO_WRITE, 0, &ready));
  HSF_CHECK(ready & HSF_IO_WRITE);

  ready = 0;
  HSF_CHECK_STATUS(t.vt->wait(t.self, HSF_IO_READ, 0, &ready), HSF_ERR_TIMEOUT);
  HSF_CHECK_EQ(ready, (uint32_t)0);

  m.PushReadable("x");
  HSF_CHECK_OK(t.vt->wait(t.self, HSF_IO_READ, 0, &ready));
  HSF_CHECK(ready & HSF_IO_READ);
}

HSF_TEST("mock: injected error surfaces after the given byte count") {
  MockTransport m;
  m.PushReadable("aaaa");
  m.PushReadable("bbbb");
  m.FailReadAfter(4, HSF_ERR_IO);
  HSFTransportRef t = m.Ref();
  HSF_REQUIRE(t.vt->open(t.self) == HSF_OK);

  char buf[4];
  size_t n = 0;
  HSF_CHECK_OK(t.vt->read(t.self, buf, sizeof(buf), &n));
  HSF_CHECK_EQ(n, (size_t)4);
  HSF_CHECK_STATUS(t.vt->read(t.self, buf, sizeof(buf), &n), HSF_ERR_IO);
}

HSF_TEST("mock: flush_input discards what has not been read") {
  MockTransport m;
  m.PushReadable("stale");
  HSFTransportRef t = m.Ref();
  HSF_REQUIRE(t.vt->open(t.self) == HSF_OK);
  HSF_CHECK_OK(t.vt->flush_input(t.self));

  char buf[16];
  size_t n = 0;
  HSF_CHECK_STATUS(t.vt->read(t.self, buf, sizeof(buf), &n), HSF_AGAIN);
}

HSF_TEST("mock: identifies as MOCK, stream-framed, with no OS handle") {
  MockTransport m;
  HSFTransportRef t = m.Ref();
  HSF_CHECK_EQ((int)t.vt->kind(t.self), (int)HSF_TRANSPORT_MOCK);
  HSF_CHECK_EQ((int)t.vt->framing(t.self), (int)HSF_FRAMING_STREAM);
  HSF_CHECK_EQ(t.vt->native_handle(t.self), (intptr_t)-1);
}

/* --- BlockingIo, the helper drivers actually use ------------------------- */

HSF_TEST("BlockingIo: WriteAll finishes a write the transport truncated") {
  MockTransport m;
  m.LimitNextWrite(2);
  HSFTransportRef t = m.Ref();
  HSF_REQUIRE(t.vt->open(t.self) == HSF_OK);

  hsf::BlockingIo io(t);
  HSF_CHECK_OK(io.WriteAll("ABCDEF", 6, 100));
  HSF_REQUIRE(m.WriteCount() == 1);
  HSF_CHECK_EQ(S(m.LastWrite()), std::string("ABCDEF"));
}

HSF_TEST("BlockingIo: ReadExact reassembles fragments") {
  MockTransport m;
  m.SetResponder([](const Bytes&) { return B("0123456789"); }, /*fragment=*/3);
  HSFTransportRef t = m.Ref();
  HSF_REQUIRE(t.vt->open(t.self) == HSF_OK);

  hsf::BlockingIo io(t);
  HSF_REQUIRE(io.WriteAll("go", 2, 100) == HSF_OK);

  char buf[10];
  HSF_CHECK_OK(io.ReadExact(buf, sizeof(buf), 100));
  HSF_CHECK_EQ(std::string(buf, 10), std::string("0123456789"));
}

HSF_TEST("BlockingIo: a short reply times out rather than returning partial") {
  /* Reporting a half-frame as success would push the error down into the
   * codec, where it looks like a protocol bug instead of a timeout. */
  MockTransport m;
  m.SetResponder([](const Bytes&) { return B("abc"); });
  HSFTransportRef t = m.Ref();
  HSF_REQUIRE(t.vt->open(t.self) == HSF_OK);

  hsf::BlockingIo io(t);
  HSF_REQUIRE(io.WriteAll("go", 2, 50) == HSF_OK);

  char buf[16];
  size_t total = 0;
  HSF_CHECK_STATUS(io.ReadAtLeast(buf, 16, 16, 20, &total), HSF_ERR_TIMEOUT);
  HSF_CHECK_EQ(total, (size_t)3);   /* reports what it did get */
}

HSF_TEST("BlockingIo: a closed peer is CLOSED, not TIMEOUT") {
  MockTransport m;
  m.CloseRemote();
  HSFTransportRef t = m.Ref();
  HSF_REQUIRE(t.vt->open(t.self) == HSF_OK);

  hsf::BlockingIo io(t);
  char buf[8];
  HSF_CHECK_STATUS(io.ReadExact(buf, sizeof(buf), 50), HSF_ERR_CLOSED);
}

HSF_TEST("BlockingIo: an unopened transport fails without waiting") {
  MockTransport m;
  hsf::BlockingIo io(m.Ref());
  char buf[4];
  HSF_CHECK_STATUS(io.WriteAll("x", 1, 10), HSF_ERR_NOT_OPEN);
  HSF_CHECK_STATUS(io.ReadExact(buf, 1, 10), HSF_ERR_NOT_OPEN);
}

HSF_TEST("BlockingIo: least > cap is rejected") {
  MockTransport m;
  HSFTransportRef t = m.Ref();
  HSF_REQUIRE(t.vt->open(t.self) == HSF_OK);
  hsf::BlockingIo io(t);
  char buf[4];
  size_t total = 0;
  HSF_CHECK_STATUS(io.ReadAtLeast(buf, 8, 4, 10, &total), HSF_ERR_INVALID_ARG);
}

HSF_TEST_MAIN()
