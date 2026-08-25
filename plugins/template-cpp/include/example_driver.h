/* Example driver — declaration.
 *
 * The protocol methods (BuildRequest, FrameLength, ParseReply) are public and
 * side-effect free on purpose: the unit tests drive them directly, with no
 * transport, no timing and no device. Keep that split when you replace them and
 * your protocol stays testable in microseconds.
 */
#ifndef EXAMPLE_DRIVER_H
#define EXAMPLE_DRIVER_H

#include "hsf/support.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace example {

using Bytes = std::vector<uint8_t>;

class ExampleDriver : public hsf::DriverBase<ExampleDriver> {
 public:
  /* --- your protocol: pure, testable, no I/O --- */
  Bytes  BuildRequest(const std::string& point) const;
  Bytes  BuildWriteRequest(const std::string& point, double value) const;
  size_t FrameLength(const Bytes& buf) const;   /* 0 = need more bytes */
  HSFStatus ParseReply(const Bytes& frame, HSFValue* out, std::string* error) const;

  /* --- the SDK lifecycle --- */
  HSFStatus Initialize(HSFConfigRef config, HSFTransportRef transport) override;
  HSFStatus Start() override;
  HSFStatus Stop() override;
  HSFStatus Read(const HSFDeviceAddress* addr, HSFValue* out) override;
  HSFStatus Write(const HSFDeviceAddress* addr, const HSFValue* value) override;
  HSFStatus Health() override;
  void      Status(HSFDriverStatus* out) override;

  /* Test seams. Public so tests can set up a driver without a config source. */
  void SetTimeoutMs(int ms) { timeout_ms_ = ms < 1 ? 1 : ms; }
  void SetRetries(int n) { retries_ = n < 0 ? 0 : n; }

 private:
  HSFStatus Transact(const Bytes& request, HSFValue* out);

  HSFTransportRef transport_{};
  hsf::BlockingIo io_{HSFTransportRef{}};
  int      timeout_ms_ = 1000;
  int      retries_ = 2;
  int      unit_id_ = 1;
  uint64_t reads_ok_ = 0;
  uint64_t reads_failed_ = 0;
  uint64_t writes_failed_ = 0;
};

}  // namespace example

#endif /* EXAMPLE_DRIVER_H */
