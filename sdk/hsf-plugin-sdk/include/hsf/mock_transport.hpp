/* HSF Plugin SDK — mock transport. Header-only, test-only, no hardware.
 *
 * Plan §19: a developer must be able to test a driver's whole protocol on a
 * laptop with nothing plugged in. This is that transport.
 *
 * It presents itself as HSF_TRANSPORT_MOCK through the SAME HSFTransportVTable
 * a real serial port or socket uses, so the driver under test cannot tell the
 * difference — which is the only way the test proves anything about production.
 *
 * What it can do that real hardware cannot do on demand, and which is the
 * reason to prefer it over a loopback socket:
 *
 *   - hand a reply over in FRAGMENTS, so a driver that assumes one read is one
 *     frame fails here instead of intermittently on site
 *   - return HSF_AGAIN on a schedule, exercising the non-blocking path that a
 *     fast loopback would otherwise never reach
 *   - inject a short write, a mid-frame close, or an I/O error at an exact byte
 *   - answer requests programmatically, so a whole simulated device is a lambda
 */
#ifndef HSF_PLUGIN_SDK_MOCK_TRANSPORT_HPP
#define HSF_PLUGIN_SDK_MOCK_TRANSPORT_HPP

#include "hsf/support.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace hsf {

class MockTransport {
 public:
  using Bytes = std::vector<uint8_t>;

  /* Answers a request the driver wrote. Return an empty vector to say nothing
   * — which is how a timeout is simulated, and the case drivers most often get
   * wrong. */
  using Responder = std::function<Bytes(const Bytes& request)>;

  MockTransport() = default;

  /* --- test-side setup --- */

  /* Queue bytes for the driver to read. Each call is one "chunk"; a chunk is
   * the most a single read() will return, so pushing a frame in two chunks
   * makes the driver reassemble. */
  void PushReadable(const Bytes& chunk) { inbox_.push_back(chunk); }
  void PushReadable(const std::string& s) {
    inbox_.push_back(Bytes(s.begin(), s.end()));
  }

  /* Answer every written frame with this. Applied after each successful write.
   * The responder's result is split into `fragment_size` chunks when that is
   * non-zero, to force reassembly. */
  void SetResponder(Responder r, size_t fragment_size = 0) {
    responder_ = std::move(r);
    fragment_ = fragment_size;
  }

  /* Return HSF_AGAIN for the next `n` reads before delivering anything. Proves
   * the driver loops on AGAIN rather than treating it as end-of-stream. */
  void StallReads(int n) { stall_reads_ = n; }

  /* Accept only `n` bytes on the next write, then AGAIN. Proves the driver
   * retries a partial write instead of shipping a truncated frame. */
  void LimitNextWrite(size_t n) { write_limit_ = n; }

  /* Fail the read that starts at or after this many delivered bytes. */
  void FailReadAfter(size_t bytes, HSFStatus st = HSF_ERR_IO) {
    fail_after_ = bytes;
    fail_status_ = st;
  }

  /* Simulate the peer closing: reads return OK with 0 bytes. */
  void CloseRemote() { remote_closed_ = true; }

  /* --- assertions --- */

  const std::vector<Bytes>& Written() const { return written_; }
  size_t WriteCount() const { return written_.size(); }
  const Bytes& LastWrite() const {
    static const Bytes empty;
    return written_.empty() ? empty : written_.back();
  }
  bool IsOpen() const { return open_; }
  size_t OpenCount() const { return open_count_; }
  void Clear() {
    inbox_.clear();
    written_.clear();
    delivered_ = 0;
  }

  /* --- the ABI face --- */

  HSFTransportRef Ref() {
    HSFTransportRef r;
    r.vt = VTable();
    r.self = reinterpret_cast<HSFTransport*>(this);
    return r;
  }

 private:
  static MockTransport* Cast(HSFTransport* t) {
    return reinterpret_cast<MockTransport*>(t);
  }
  static const MockTransport* Cast(const HSFTransport* t) {
    return reinterpret_cast<const MockTransport*>(t);
  }

  static const HSFTransportVTable* VTable() {
    static const HSFTransportVTable vt = Make();
    return &vt;
  }

  static HSFTransportVTable Make() {
    HSFTransportVTable vt{};
    vt.struct_size = static_cast<uint32_t>(sizeof(HSFTransportVTable));

    vt.kind = [](const HSFTransport*) { return HSF_TRANSPORT_MOCK; };
    vt.framing = [](const HSFTransport*) { return HSF_FRAMING_STREAM; };

    vt.open = [](HSFTransport* t) -> HSFStatus {
      MockTransport* m = Cast(t);
      if (m->open_) return HSF_ERR_ALREADY_OPEN;
      m->open_ = true;
      ++m->open_count_;
      return HSF_OK;
    };
    vt.close = [](HSFTransport* t) { Cast(t)->open_ = false; };
    vt.is_open = [](const HSFTransport* t) {
      return Cast(t)->open_ ? (int32_t)1 : (int32_t)0;
    };

    vt.read = [](HSFTransport* t, void* buf, size_t cap,
                 size_t* transferred) -> HSFStatus {
      MockTransport* m = Cast(t);
      if (transferred) *transferred = 0;
      if (!m->open_) return HSF_ERR_NOT_OPEN;
      if (!buf || cap == 0) return HSF_ERR_INVALID_ARG;

      if (m->fail_after_ >= 0 &&
          m->delivered_ >= static_cast<size_t>(m->fail_after_)) {
        return m->fail_status_;
      }
      if (m->stall_reads_ > 0) {
        --m->stall_reads_;
        return HSF_AGAIN;
      }
      if (m->inbox_.empty()) {
        /* Closed beats AGAIN: a driver must distinguish "nothing yet" from
         * "never again", and if both were AGAIN it could not. */
        return m->remote_closed_ ? HSF_OK : HSF_AGAIN;
      }

      Bytes& front = m->inbox_.front();
      const size_t n = front.size() < cap ? front.size() : cap;
      std::memcpy(buf, front.data(), n);
      front.erase(front.begin(), front.begin() + static_cast<long>(n));
      if (front.empty()) m->inbox_.pop_front();
      m->delivered_ += n;
      if (transferred) *transferred = n;
      return HSF_OK;
    };

    vt.write = [](HSFTransport* t, const void* buf, size_t len,
                  size_t* transferred) -> HSFStatus {
      MockTransport* m = Cast(t);
      if (transferred) *transferred = 0;
      if (!m->open_) return HSF_ERR_NOT_OPEN;
      if (!buf || len == 0) return HSF_ERR_INVALID_ARG;

      size_t accept = len;
      if (m->write_limit_ > 0 && m->write_limit_ < len) {
        accept = m->write_limit_;
        m->write_limit_ = 0;  /* one-shot */
      }

      const uint8_t* p = static_cast<const uint8_t*>(buf);
      m->partial_.insert(m->partial_.end(), p, p + accept);
      if (transferred) *transferred = accept;

      /* A frame is complete when the whole offered buffer was taken. Good
       * enough for a mock: a driver retrying a partial write finishes it on the
       * next call, and only then does the responder see one whole frame. */
      if (accept == len) {
        m->written_.push_back(m->partial_);
        Bytes request = m->partial_;
        m->partial_.clear();
        if (m->responder_) {
          Bytes reply = m->responder_(request);
          if (!reply.empty()) m->Enqueue(reply);
        }
      }
      return HSF_OK;
    };

    vt.wait = [](HSFTransport* t, uint32_t events, int32_t /*timeout_ms*/,
                 uint32_t* ready) -> HSFStatus {
      MockTransport* m = Cast(t);
      uint32_t r = 0;
      /* No real waiting: a mock is always write-ready, and readable exactly
       * when something is queued. A test that needs a timeout arranges an
       * empty inbox and gets HSF_ERR_TIMEOUT here, with no wall-clock cost —
       * which is why the whole suite runs in milliseconds. */
      if ((events & HSF_IO_READ) && !m->inbox_.empty()) r |= HSF_IO_READ;
      if (events & HSF_IO_WRITE) r |= HSF_IO_WRITE;
      if (m->remote_closed_ && m->inbox_.empty()) r |= HSF_IO_CLOSED;
      if (ready) *ready = r;
      if (r == 0) return HSF_ERR_TIMEOUT;
      return HSF_OK;
    };

    vt.flush_input = [](HSFTransport* t) -> HSFStatus {
      Cast(t)->inbox_.clear();
      return HSF_OK;
    };
    vt.native_handle = [](const HSFTransport*) { return (intptr_t)-1; };
    vt.last_error = [](const HSFTransport* t) {
      return Borrow(Cast(t)->last_error_);
    };
    return vt;
  }

  void Enqueue(const Bytes& data) {
    if (fragment_ == 0 || data.size() <= fragment_) {
      inbox_.push_back(data);
      return;
    }
    for (size_t off = 0; off < data.size(); off += fragment_) {
      const size_t n = (data.size() - off) < fragment_ ? (data.size() - off) : fragment_;
      inbox_.push_back(Bytes(data.begin() + static_cast<long>(off),
                             data.begin() + static_cast<long>(off + n)));
    }
  }

  std::deque<Bytes>  inbox_;
  std::vector<Bytes> written_;
  Bytes              partial_;
  Responder          responder_;
  std::string        last_error_;

  bool     open_ = false;
  size_t   open_count_ = 0;
  bool     remote_closed_ = false;
  int      stall_reads_ = 0;
  size_t   write_limit_ = 0;
  size_t   fragment_ = 0;
  size_t   delivered_ = 0;
  long     fail_after_ = -1;
  HSFStatus fail_status_ = HSF_ERR_IO;
};

}  // namespace hsf

#endif /* HSF_PLUGIN_SDK_MOCK_TRANSPORT_HPP */
