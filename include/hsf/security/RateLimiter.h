#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hsf {

// Token-bucket rate limiting (request/AdvanceUpdate.md section 1.4).
//
// A bucket per key holds up to `burst` tokens and refills at `perSecond`. Each
// request takes one; an empty bucket means HTTP 429. Token bucket rather than
// a fixed window because a fixed window lets a client spend its whole quota in
// the last millisecond of one window and again in the first of the next --
// twice the intended rate, right at the boundary.
//
// IN-MEMORY, NOT IN SQLITE, unlike the login lockout next door. The two look
// similar and are deliberately different: a lockout must survive a restart, or
// an attacker just waits for one. A rate limit is about instantaneous load,
// and persisting a counter that changes on literally every request would put a
// disk write in front of every API call on a device with an SD card.
//
// The map is bounded. Without that, keying by client IP is itself the denial
// of service: an attacker with a large address range makes the gateway
// allocate a bucket per source until it runs out of memory. When the cap is
// reached the idle buckets are swept, and if that frees nothing the request is
// allowed rather than denied -- a rate limiter that fails closed under
// pressure would take the machine down on behalf of the attacker.
class RateLimiter {
 public:
  // `perSecond` tokens/second sustained, `burst` maximum accumulated.
  RateLimiter(double perSecond, double burst, size_t maxKeys = 4096);

  // Takes a token for `key`. False means the caller should answer 429.
  bool Allow(const std::string& key);

  // Seconds until at least one token is available, for the Retry-After header.
  double RetryAfter(const std::string& key);

  void Reset(const std::string& key);
  void Clear();

  void Configure(double perSecond, double burst);

  size_t TrackedKeys() const;

 private:
  struct Bucket {
    double tokens = 0.0;
    std::chrono::steady_clock::time_point last;
  };

  // Callers hold mutex_.
  Bucket& TouchUnlocked(const std::string& key);
  void SweepUnlocked();

  mutable std::mutex mutex_;
  double perSecond_;
  double burst_;
  size_t maxKeys_;
  std::unordered_map<std::string, Bucket> buckets_;
};

}  // namespace hsf
