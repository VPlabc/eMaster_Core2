/* HSF Plugin SDK — C++ conveniences. HEADER-ONLY, and NOT part of the ABI.
 *
 * Nothing in this file crosses the plugin boundary. It exists so that writing a
 * plugin in C++ does not mean writing C: the ABI stays austere, and the
 * ergonomics live here where they cost nothing at runtime and nothing in
 * compatibility. A plugin may ignore this file entirely.
 */
#ifndef HSF_PLUGIN_SDK_SUPPORT_HPP
#define HSF_PLUGIN_SDK_SUPPORT_HPP

#include "hsf/plugin.h"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace hsf {

/* --- strings ------------------------------------------------------------- */

inline std::string ToString(HSFStr s) {
  return (s.ptr && s.len) ? std::string(s.ptr, s.len) : std::string();
}

/* Borrows the std::string's buffer. The string must outlive every use of the
 * result — the commonest ABI mistake in this whole SDK is handing out an HSFStr
 * pointing into a temporary, so name the owner and keep it alive. */
inline HSFStr Borrow(const std::string& s) {
  HSFStr r;
  r.ptr = s.c_str();
  r.len = s.size();
  return r;
}

/* --- exception guard ----------------------------------------------------- */

/* Wrap EVERY plugin entry point in this.
 *
 * An exception unwinding out of a plugin and into the host is undefined
 * behaviour: the two sides may be built by different compilers with different
 * unwind tables, and even when they are not, the host has no catch handler
 * shaped for a type it has never seen. In practice it terminates the gateway —
 * so a driver with a bad std::stoi takes down every other protocol with it.
 *
 * Returns a status instead. `fn` returns HSFStatus. */
template <typename Fn>
inline HSFStatus Guard(Fn&& fn) noexcept {
  try {
    return fn();
  } catch (const std::bad_alloc&) {
    return HSF_ERR_NO_MEMORY;
  } catch (const std::exception&) {
    return HSF_ERR_INTERNAL;
  } catch (...) {
    return HSF_ERR_UNKNOWN;
  }
}

/* Void form, for slots that cannot report failure (destroy, status). */
template <typename Fn>
inline void GuardVoid(Fn&& fn) noexcept {
  try {
    fn();
  } catch (...) {
    /* Swallowed on purpose: the slot has no way to say so, and letting it
     * escape is worse than losing the detail. Log inside `fn` if it matters. */
  }
}

/* --- blocking I/O over the non-blocking transport ------------------------ */

/* The transport ABI is readiness-based so the host CAN run every driver on one
 * event loop (see transport.h). But a request/response protocol is far clearer
 * written as "send, then read the reply", and a driver that owns a thread is a
 * legitimate choice. This turns the non-blocking interface into blocking calls
 * without putting that requirement in the ABI.
 *
 * Use it only from a driver that declared owns_thread — calling it from the
 * host's event loop would block every other driver sharing that loop. */
class BlockingIo {
 public:
  explicit BlockingIo(HSFTransportRef t) : t_(t) {}

  /* Writes all `len` bytes or fails. Partial writes are retried, because a
   * partially-written frame is a protocol error at the far end, not a smaller
   * frame. */
  HSFStatus WriteAll(const void* data, size_t len, int timeout_ms) {
    if (!hsf_transport_valid(t_)) return HSF_ERR_NOT_OPEN;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    Deadline deadline(timeout_ms);
    while (sent < len) {
      size_t n = 0;
      HSFStatus st = t_.vt->write(t_.self, p + sent, len - sent, &n);
      if (st == HSF_AGAIN) {
        HSFStatus w = Await(HSF_IO_WRITE, deadline);
        if (w < 0) return w;
        continue;
      }
      if (st < 0) return st;
      if (n == 0) return HSF_ERR_CLOSED;
      sent += n;
    }
    return HSF_OK;
  }

  /* Reads exactly `len` bytes. Short of that within the deadline is
   * HSF_ERR_TIMEOUT, not a partial success — a half-read frame cannot be
   * parsed, so reporting it as OK would push the error into the codec. */
  HSFStatus ReadExact(void* buf, size_t len, int timeout_ms) {
    size_t got = 0;
    return ReadAtLeast(buf, len, len, timeout_ms, &got);
  }

  /* Reads between `least` and `cap` bytes. For a protocol whose frame length is
   * only known after a header has arrived. */
  HSFStatus ReadAtLeast(void* buf, size_t least, size_t cap, int timeout_ms,
                        size_t* out_total) {
    if (out_total) *out_total = 0;
    if (!hsf_transport_valid(t_)) return HSF_ERR_NOT_OPEN;
    if (least > cap) return HSF_ERR_INVALID_ARG;
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t total = 0;
    Deadline deadline(timeout_ms);
    while (total < least) {
      size_t n = 0;
      HSFStatus st = t_.vt->read(t_.self, p + total, cap - total, &n);
      if (st == HSF_AGAIN) {
        HSFStatus w = Await(HSF_IO_READ, deadline);
        if (w < 0) {
          if (out_total) *out_total = total;
          return w;
        }
        continue;
      }
      if (st < 0) {
        if (out_total) *out_total = total;
        return st;
      }
      if (n == 0) {
        if (out_total) *out_total = total;
        return HSF_ERR_CLOSED;  /* stream ended: distinct from HSF_AGAIN */
      }
      total += n;
    }
    if (out_total) *out_total = total;
    return HSF_OK;
  }

 private:
  /* Monotonic, so a clock adjustment mid-poll cannot make a 200 ms timeout wait
   * an hour. */
  class Deadline {
   public:
    explicit Deadline(int timeout_ms)
        : infinite_(timeout_ms < 0),
          at_(std::chrono::steady_clock::now() +
              std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms)) {}

    int RemainingMs() const {
      if (infinite_) return -1;
      auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                      at_ - std::chrono::steady_clock::now())
                      .count();
      return left <= 0 ? 0 : static_cast<int>(left);
    }
    bool Expired() const { return !infinite_ && RemainingMs() == 0; }

   private:
    bool infinite_;
    std::chrono::steady_clock::time_point at_;
  };

  HSFStatus Await(uint32_t events, const Deadline& deadline) {
    if (deadline.Expired()) return HSF_ERR_TIMEOUT;
    if (!t_.vt->wait) return HSF_ERR_NOT_SUPPORTED;
    uint32_t ready = 0;
    HSFStatus st = t_.vt->wait(t_.self, events, deadline.RemainingMs(), &ready);
    if (st < 0) return st;
    if (ready & HSF_IO_CLOSED) return HSF_ERR_CLOSED;
    if (ready & HSF_IO_ERROR) return HSF_ERR_IO;
    if ((ready & events) == 0) return HSF_ERR_TIMEOUT;
    return HSF_OK;
  }

  HSFTransportRef t_;
};

/* --- driver base --------------------------------------------------------- */

/* Bridges a C++ class to the HSFDriverVTable without the author writing a
 * single trampoline. Derive, override what applies, and hand VTable() and
 * Self() to the host.
 *
 * Every trampoline is Guard()ed, so an exception in a driver method becomes a
 * status code rather than a dead gateway. */
template <typename Derived>
class DriverBase {
 public:
  virtual ~DriverBase() = default;

  /* Defaults are honest refusals rather than silent successes: a driver that
   * forgets to implement write should fail visibly, not appear to work. */
  virtual HSFStatus Initialize(HSFConfigRef, HSFTransportRef) { return HSF_OK; }
  virtual HSFStatus Start() { return HSF_OK; }
  virtual HSFStatus Stop() { return HSF_OK; }
  virtual HSFStatus Read(const HSFDeviceAddress*, HSFValue*) { return HSF_ERR_NOT_SUPPORTED; }
  virtual HSFStatus Write(const HSFDeviceAddress*, const HSFValue*) { return HSF_ERR_NOT_SUPPORTED; }
  virtual HSFStatus Health() { return HSF_ERR_NOT_SUPPORTED; }
  virtual void Status(HSFDriverStatus* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->struct_size = static_cast<uint32_t>(sizeof(HSFDriverStatus));
    out->state = state_;
    out->connected = connected_ ? 1 : 0;
    out->last_error = Borrow(last_error_);
  }

  HSFDriver* Self() { return reinterpret_cast<HSFDriver*>(this); }

  static const HSFDriverVTable* VTable() {
    static const HSFDriverVTable vt = MakeVTable();
    return &vt;
  }

 protected:
  void SetState(HSFDriverState s) { state_ = s; }
  void SetConnected(bool c) { connected_ = c; }
  void SetError(std::string e) { last_error_ = std::move(e); }

  HSFDriverState state_ = HSF_DRIVER_CREATED;
  bool           connected_ = false;
  std::string    last_error_;

 private:
  static Derived* Cast(HSFDriver* d) { return reinterpret_cast<Derived*>(d); }

  static HSFDriverVTable MakeVTable() {
    HSFDriverVTable vt{};
    vt.struct_size = static_cast<uint32_t>(sizeof(HSFDriverVTable));
    vt.initialize = [](HSFDriver* d, HSFConfigRef c, HSFTransportRef t) {
      return Guard([&] { return Cast(d)->Initialize(c, t); });
    };
    vt.start = [](HSFDriver* d) { return Guard([&] { return Cast(d)->Start(); }); };
    vt.stop  = [](HSFDriver* d) { return Guard([&] { return Cast(d)->Stop(); }); };
    vt.read = [](HSFDriver* d, const HSFDeviceAddress* a, HSFValue* v) {
      return Guard([&] { return Cast(d)->Read(a, v); });
    };
    vt.write = [](HSFDriver* d, const HSFDeviceAddress* a, const HSFValue* v) {
      return Guard([&] { return Cast(d)->Write(a, v); });
    };
    vt.health = [](HSFDriver* d) { return Guard([&] { return Cast(d)->Health(); }); };
    vt.status = [](const HSFDriver* d, HSFDriverStatus* out) {
      GuardVoid([&] { Cast(const_cast<HSFDriver*>(d))->Status(out); });
    };
    /* read_many/write_many/discover/on_readable/on_tick/command stay null:
     * the ABI documents null as a complete answer, and the host falls back. */
    return vt;
  }
};

}  // namespace hsf

#endif /* HSF_PLUGIN_SDK_SUPPORT_HPP */
