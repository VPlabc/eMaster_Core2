#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace hsf {

// SHA-256 (FIPS 180-4), implemented here rather than pulled from OpenSSL.
//
// The signature check below it genuinely needs a crypto library, and that one
// is optional (see SignatureVerifier). The checksum is not optional: a package
// whose bytes do not match the manifest must be rejected on every build,
// including one configured without OpenSSL. Hashing is also the one primitive
// where a from-scratch implementation is defensible -- it has no key, no
// randomness and no timing surface, and it is verified against the published
// FIPS test vectors in docs/ota-update.md.
class Sha256 {
 public:
  Sha256() { Reset(); }

  void Reset();
  void Update(const void* data, size_t length);
  // Lowercase hex, 64 characters. The object is finished after this.
  std::string HexDigest();

  // Convenience wrappers.
  static std::string HexOf(const std::string& data);
  // Empty string on a file that cannot be read -- callers treat that as a
  // failed verification, never as "no hash needed".
  static std::string HexOfFile(const std::string& path);
  // Raw 32-byte digest of a file, for the signature check (which signs the
  // digest, not the archive).
  static bool RawOfFile(const std::string& path, uint8_t out[32]);

  // Case-insensitive comparison of two hex digests, constant time in the
  // length of the digest. Not because a checksum is a secret, but because
  // "compare hashes with ==" is the habit that eventually gets applied to
  // something that is.
  static bool HexEquals(const std::string& a, const std::string& b);

 private:
  void Transform(const uint8_t block[64]);

  uint32_t state_[8];
  uint64_t bitCount_;
  uint8_t buffer_[64];
  size_t bufferLength_;
};

}  // namespace hsf
