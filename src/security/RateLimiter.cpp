#include "hsf/security/RateLimiter.h"

#include <algorithm>
#include <vector>

namespace hsf {
namespace {
// A bucket untouched for this long is at full capacity by definition, so
// forgetting it changes no decision -- it just reclaims the memory.
constexpr auto kIdleEviction = std::chrono::minutes(10);
}  // namespace

RateLimiter::RateLimiter(double perSecond, double burst, size_t maxKeys)
    : perSecond_(perSecond > 0 ? perSecond : 1.0),
      burst_(burst > 0 ? burst : 1.0),
      maxKeys_(maxKeys > 0 ? maxKeys : 1024) {}

void RateLimiter::Configure(double perSecond, double burst) {
  std::lock_guard<std::mutex> lock(mutex_);
  perSecond_ = perSecond > 0 ? perSecond : 1.0;
  burst_ = burst > 0 ? burst : 1.0;
  // Existing buckets keep their current level; it is clamped on next use.
}

void RateLimiter::SweepUnlocked() {
  const auto now = std::chrono::steady_clock::now();
  for (auto it = buckets_.begin(); it != buckets_.end();) {
    it = (now - it->second.last > kIdleEviction) ? buckets_.erase(it) : std::next(it);
  }
}

RateLimiter::Bucket& RateLimiter::TouchUnlocked(const std::string& key) {
  const auto now = std::chrono::steady_clock::now();

  auto it = buckets_.find(key);
  if (it == buckets_.end()) {
    if (buckets_.size() >= maxKeys_) {
      SweepUnlocked();
      if (buckets_.size() >= maxKeys_) {
        // Still full: evict the single least recently used entry rather than
        // refusing to track this one. Bounded work, and the alternative
        // (denying the request) is the attacker's goal.
        auto oldest = buckets_.begin();
        for (auto candidate = buckets_.begin(); candidate != buckets_.end(); ++candidate) {
          if (candidate->second.last < oldest->second.last) oldest = candidate;
        }
        buckets_.erase(oldest);
      }
    }
    Bucket fresh;
    fresh.tokens = burst_;
    fresh.last = now;
    it = buckets_.emplace(key, fresh).first;
    return it->second;
  }

  Bucket& bucket = it->second;
  const double elapsed = std::chrono::duration<double>(now - bucket.last).count();
  bucket.tokens = std::min(burst_, bucket.tokens + elapsed * perSecond_);
  bucket.last = now;
  return bucket;
}

bool RateLimiter::Allow(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  Bucket& bucket = TouchUnlocked(key);
  if (bucket.tokens < 1.0) return false;
  bucket.tokens -= 1.0;
  return true;
}

double RateLimiter::RetryAfter(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  Bucket& bucket = TouchUnlocked(key);
  if (bucket.tokens >= 1.0) return 0.0;
  return (1.0 - bucket.tokens) / perSecond_;
}

void RateLimiter::Reset(const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  buckets_.erase(key);
}

void RateLimiter::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  buckets_.clear();
}

size_t RateLimiter::TrackedKeys() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return buckets_.size();
}

}  // namespace hsf
