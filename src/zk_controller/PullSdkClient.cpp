#include "hsf/zk_controller/PullSdkClient.h"

#include "plcommpro.h"

// x86-only alias fixups for the 5 native functions this trimmed wrapper
// actually calls -- see ZKTecoProtocol/src/pullsdk/pullsdk_client.cpp for
// the full 16-function version and "Design decision D4" in its CMakeLists
// for why these exist at all (MSVC x86 __stdcall decoration vs. the
// hand-written plcommpro.def's plain names).
#if defined(_M_IX86)
#pragma comment(linker, "/alternatename:_Connect@4=_Connect")
#pragma comment(linker, "/alternatename:_Disconnect@4=_Disconnect")
#pragma comment(linker, "/alternatename:_PullLastError@0=_PullLastError")
#pragma comment(linker, "/alternatename:_GetDeviceParam@16=_GetDeviceParam")
#pragma comment(linker, "/alternatename:_SetDeviceParam@8=_SetDeviceParam")
#pragma comment(linker, "/alternatename:_ControlDevice@28=_ControlDevice")
#pragma comment(linker, "/alternatename:_GetRTLog@12=_GetRTLog")
#endif

namespace hsf {

PullSdkClient::~PullSdkClient() { Disconnect(); }

bool PullSdkClient::Connect(const std::string& connectionString) {
  handle_ = ::Connect(connectionString.c_str());
  return handle_ != nullptr;
}

void PullSdkClient::Disconnect() {
  if (handle_ != nullptr) {
    ::Disconnect(handle_);
    handle_ = nullptr;
  }
}

bool PullSdkClient::IsConnected() const { return handle_ != nullptr; }

PullError PullSdkClient::LastError() { return static_cast<PullError>(::PullLastError()); }

std::string PullSdkClient::GetDeviceParam(const std::string& items, int bufferSize, int* retOut) {
  std::string buffer(static_cast<size_t>(bufferSize), '\0');
  int ret = ::GetDeviceParam(handle_, buffer.data(), bufferSize, items.c_str());
  if (retOut != nullptr) *retOut = ret;
  return buffer;
}

int PullSdkClient::SetDeviceParam(const std::string& itemValues) {
  if (handle_ == nullptr) return -1;
  return ::SetDeviceParam(handle_, itemValues.c_str());
}

int PullSdkClient::ControlDevice(int operationId, int param1, int param2, int param3, int param4,
                                  const std::string& options) {
  if (handle_ == nullptr) return -1;
  return ::ControlDevice(handle_, operationId, param1, param2, param3, param4, options.c_str());
}

std::string PullSdkClient::GetRTLog(int bufferSize, int* retOut) {
  std::string buffer(static_cast<size_t>(bufferSize), '\0');
  int ret = ::GetRTLog(handle_, buffer.data(), bufferSize);
  if (retOut != nullptr) *retOut = ret;
  return buffer;
}

}  // namespace hsf
