#pragma once

#include "hsf/support.hpp"

#include <cstdint>
#include <vector>

namespace hsf_c3protocol {

class C3Driver : public hsf::DriverBase<C3Driver> {
 public:
  HSFStatus Initialize(HSFConfigRef config, HSFTransportRef transport) override;
  HSFStatus Start() override;
  HSFStatus Stop() override;
  HSFStatus Read(const HSFDeviceAddress* address, HSFValue* out) override;
  HSFStatus Write(const HSFDeviceAddress* address, const HSFValue* value) override;
  HSFStatus Health() override;
  void Status(HSFDriverStatus* out) override;

 private:
  HSFStatus Exchange(const std::vector<uint8_t>& request, std::vector<uint8_t>* reply);
  HSFStatus ConnectSessionLess();

  HSFTransportRef transport_{};
  hsf::BlockingIo io_{HSFTransportRef{}};
  int timeout_ms_ = 2000;
  uint64_t health_ok_ = 0;
  uint64_t health_failed_ = 0;
};

}  // namespace hsf_c3protocol
