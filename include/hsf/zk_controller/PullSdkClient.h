#pragma once

#include <string>

#include "hsf/zk_controller/PullSdkTypes.h"

namespace hsf {

// The only class that calls the raw plcommpro.dll C ABI. Non-copyable, RAII
// (auto-disconnects in destructor). Ported from
// ZKTecoProtocol/src/pullsdk/pullsdk_client.h, trimmed to the methods
// ZkController actually needs (connect/heartbeat/RTLog poll) -- see that
// file for the full original wrapper (setDeviceParam/controlDevice/device
// data/search device/file transfer/etc.), none of which AppState or
// RTLogPoller ever called.
//
// Connection-string params (ip/port/timeout/password) and "~SerialNumber"
// are all-ASCII in practice, so plain std::string bytes go straight to the
// native char* ABI with no codepage conversion -- unlike the original
// project's device-name/param calls, which need explicit ANSI (not UTF-8)
// conversion via QString::toLocal8Bit(); see fastRead.md if this wrapper is
// ever extended to cover those.
class PullSdkClient {
 public:
  PullSdkClient() = default;
  ~PullSdkClient();
  PullSdkClient(const PullSdkClient&) = delete;
  PullSdkClient& operator=(const PullSdkClient&) = delete;

  bool Connect(const std::string& connectionString);
  void Disconnect();
  bool IsConnected() const;
  static PullError LastError();

  // items e.g. "~SerialNumber" -- used as the heartbeat probe. Also
  // "LockCount,AuxOutCount,AuxInCount,ReaderCount", which is how the gateway
  // learns how many relays and inputs this panel actually has.
  std::string GetDeviceParam(const std::string& items, int bufferSize, int* retOut = nullptr);

  // "Door1Drivertime=6,Door1SensorType=2" -- comma-separated Field=Value, per
  // sdk-protocol-reference.md section 3.4/Attached Table 2. Returns the SDK's
  // own return code (>= 0 on success).
  int SetDeviceParam(const std::string& itemValues);

  // ControlDevice (sdk-protocol-reference.md section 3.5 / Attached Table 3):
  //
  //   operationId 1 = output operation. param1 = door number OR auxiliary
  //                   output number, chosen by param2 (1 = door output,
  //                   2 = auxiliary output); param3 = 0 disable, 1..60 hold
  //                   that many SECONDS, 255 latch open ("normal open state").
  //   operationId 2 = cancel alarm
  //   operationId 3 = restart device
  //   operationId 4 = enable/disable normal-open state (param1 = door,
  //                   param2 = 1 enable / 0 disable)
  //
  // Returns the SDK's return code: >= 0 success, negative is a PullError.
  int ControlDevice(int operationId, int param1, int param2, int param3, int param4,
                     const std::string& options = std::string());

  std::string GetRTLog(int bufferSize, int* retOut = nullptr);

 private:
  void* handle_ = nullptr;
};

}  // namespace hsf
