#pragma once

#include "hsf/support.hpp"

#include <cstdint>
#include <vector>

namespace hsf_modbus {

// `a1` identifies the Modbus table (1 coils, 2 discrete inputs, 3 holding
// registers, 4 input registers); `a2` is its zero-based address.  This keeps
// Modbus-specific addressing in the plugin, not in the gateway Device API.
class ModbusDriver : public hsf::DriverBase<ModbusDriver> {
 public:
  HSFStatus Initialize(HSFConfigRef config, HSFTransportRef transport) override;
  HSFStatus Start() override;
  HSFStatus Stop() override;
  HSFStatus Read(const HSFDeviceAddress* address, HSFValue* out) override;
  HSFStatus Write(const HSFDeviceAddress* address, const HSFValue* value) override;
  HSFStatus Health() override;
  void Status(HSFDriverStatus* out) override;

 private:
  HSFStatus Exchange(uint8_t function, const std::vector<uint8_t>& request,
                     std::vector<uint8_t>* response);
  HSFStatus ReadBits(const HSFDeviceAddress& address, HSFValue* out);
  HSFStatus ReadRegister(const HSFDeviceAddress& address, HSFValue* out);

  HSFTransportRef transport_{};
  hsf::BlockingIo io_{HSFTransportRef{}};
  uint16_t transaction_ = 0;
  int timeout_ms_ = 2000;
  int unit_id_ = 1;
  uint64_t reads_ok_ = 0, reads_failed_ = 0, writes_ok_ = 0, writes_failed_ = 0;
};

}  // namespace hsf_modbus
