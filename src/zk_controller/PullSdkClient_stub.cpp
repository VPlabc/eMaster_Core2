// Stand-in for PullSdkClient on platforms where the ZKTeco PullSDK is not
// available. Selected by CMake instead of PullSdkClient.cpp whenever
// HSF_ENABLE_ZK is OFF (see CMakeLists.txt).
//
// Why a stub rather than #ifdef-ing out everything above it: ZkController,
// the Lua zk.* bindings, the dashboard status card and main.cpp's wiring all
// sit on top of this one class, and conditionally compiling them out would
// mean ~70 #ifdefs across LuaEngine, WebServer and main -- and would make
// zk.connect() a nil-call error on Linux, which reads like a broken build
// rather than an unsupported feature. Keeping the whole stack compiled and
// failing here means a script gets an honest "not supported on this
// platform" at the one place that can actually say so.
//
// plcommpro.dll is a 32-bit Windows-only binary shipped by ZKTeco; nothing
// in this repository can rebuild it or port it, so this is a hard platform
// boundary, not a temporary gap.

#include "hsf/zk_controller/PullSdkClient.h"

#include "hsf/Logger.h"

namespace hsf {

namespace {
// Logged once per process rather than per call: a polling ZkController would
// otherwise fill the log with the same line every few seconds.
void WarnOnce() {
  static bool warned = false;
  if (warned) return;
  warned = true;
  Logger::Instance().Warning(
      LogCategory::System,
      "ZKTeco controller support is not built into this binary. It requires the PullSDK "
      "(plcommpro.dll), which is 32-bit Windows only -- use the Windows x86 build if you need "
      "ZK card reading. Every zk.* call will fail until then.");
}
}  // namespace

PullSdkClient::~PullSdkClient() = default;

bool PullSdkClient::Connect(const std::string&) {
  WarnOnce();
  return false;
}

void PullSdkClient::Disconnect() { handle_ = nullptr; }

bool PullSdkClient::IsConnected() const { return false; }

PullError PullSdkClient::LastError() {
  // Distinct from any real PullSDK code, so "unsupported" is never mistaken
  // for a device or network fault the operator could act on.
  return PullError::NotSupported;
}

std::string PullSdkClient::GetDeviceParam(const std::string&, int, int* retOut) {
  WarnOnce();
  if (retOut) *retOut = -1;
  return std::string();
}

int PullSdkClient::SetDeviceParam(const std::string&) {
  WarnOnce();
  return -1;
}

int PullSdkClient::ControlDevice(int, int, int, int, int, const std::string&) {
  WarnOnce();
  return -1;
}

std::string PullSdkClient::GetRTLog(int, int* retOut) {
  WarnOnce();
  if (retOut) *retOut = -1;
  return std::string();
}

}  // namespace hsf
