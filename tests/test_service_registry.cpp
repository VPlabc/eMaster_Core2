#include "hsf/ServiceRegistry.h"

#include <iostream>
#include <string>

namespace {

struct TestService {
  int value = 0;
};

struct OtherService {};

int failures = 0;

void Check(bool condition, const char* description) {
  if (condition) {
    std::cout << "ok    " << description << '\n';
    return;
  }
  std::cerr << "FAIL  " << description << '\n';
  ++failures;
}

}  // namespace

int main() {
  hsf::ServiceRegistry registry;
  TestService first{7};
  TestService replacement{11};

  registry.Register("test", &first, "test.capability");
  Check(registry.Get<TestService>("test") == &first,
        "registration resolves by name and exact type");
  Check(registry.Get<OtherService>("test") == nullptr,
        "a mismatched type is rejected");
  Check(registry.Get<TestService>("missing") == nullptr,
        "an unknown name resolves to null");
  Check(registry.FindByCapability<TestService>("test.capability") == &first,
        "registration resolves by capability and exact type");
  Check(registry.FindByCapability<OtherService>("test.capability") == nullptr,
        "capability lookup rejects a mismatched type");

  Check(registry.SetLifecycle("test", hsf::ServiceLifecycleState::kRunning),
        "lifecycle can be updated for a registered service");
  Check(registry.SetHealth("test", hsf::ServiceHealthState::kHealthy),
        "health can be updated for a registered service");
  Check(!registry.SetLifecycle("missing", hsf::ServiceLifecycleState::kRunning),
        "lifecycle update rejects an unknown service");
  Check(!registry.SetHealth("missing", hsf::ServiceHealthState::kHealthy),
        "health update rejects an unknown service");

  auto records = registry.List();
  Check(records.size() == 1, "list returns every registered service once");
  Check(records.size() == 1 &&
            records[0].lifecycle == hsf::ServiceLifecycleState::kRunning &&
            records[0].health == hsf::ServiceHealthState::kHealthy,
        "list exposes the current lifecycle and health state");

  registry.Register("test", &replacement, "test.replacement",
                    hsf::ServiceLifecycleState::kConfigured,
                    hsf::ServiceHealthState::kDegraded);
  records = registry.List();
  Check(registry.Get<TestService>("test") == &replacement && records.size() == 1,
        "registering an existing name replaces its record atomically");
  Check(registry.FindByCapability<TestService>("test.capability") == nullptr &&
            registry.FindByCapability<TestService>("test.replacement") == &replacement,
        "replacement removes the old capability mapping");

  std::cout << '\n' << (13 - failures) << " passed, " << failures << " failed\n";
  return failures == 0 ? 0 : 1;
}
