// Built instead of MqClient.cpp when HSF_ENABLE_MQ is off -- i.e. when the
// AMQP-CPP library isn't available for this triplet. Same idea as
// zk_controller/PullSdkClient_stub.cpp: the Mq.* Lua API, the `mq`
// configuration section and the Configuration page stay exactly where they are,
// and every call fails honestly instead of the whole feature vanishing from the
// API (which would turn a missing dependency into "attempt to index a nil
// value" inside somebody's workflow script).
//
// The envelope codec is NOT stubbed: it lives in MqCodec.cpp, needs no broker
// and no library, and is compiled either way.

#include <mutex>

#include "hsf/Logger.h"
#include "hsf/MqClient.h"

namespace hsf {

namespace {
constexpr const char* kUnavailable =
    "RabbitMQ support is not compiled in (build with HSF_ENABLE_MQ=ON and the amqpcpp dependency)";
}

struct MqClient::Impl {
  mutable std::mutex mutex;
  MqConfig config;
  bool warned = false;
};

MqClient::MqClient() : impl_(std::make_unique<Impl>()) {}
MqClient::~MqClient() = default;

void MqClient::Configure(const MqConfig& config) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->config = config;

  // Warned once, and only where it matters: a gateway with mq.enabled off is
  // not missing anything, so it gets no noise.
  if (config.enabled && !impl_->warned) {
    impl_->warned = true;
    Logger::Instance().Warning(LogCategory::Mq, std::string("mq.enabled is on but ") + kUnavailable);
  }
}

MqConfig MqClient::GetConfig() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->config;
}

void MqClient::SetMessageCallback(MessageCallback) {}

void MqClient::Start() {}
void MqClient::Stop() {}

bool MqClient::IsConnected() const { return false; }

MqStatus MqClient::Status() const {
  MqStatus status;
  status.enabled = GetConfig().enabled;
  status.error = kUnavailable;
  return status;
}

bool MqClient::Publish(const std::string&, const std::string&, const std::string&, bool, std::string& error) {
  error = kUnavailable;
  return false;
}

bool MqClient::Flush(int) { return true; }

void MqClient::Pause(bool) {}
bool MqClient::IsPaused() const { return false; }

bool MqClient::Available() const { return false; }
bool MqClient::Get(MqMessage&) { return false; }
void MqClient::Clear() {}

bool MqClient::TestConnect(const MqConfig&, std::string& error) {
  error = kUnavailable;
  return false;
}

}  // namespace hsf
