#include "hsf/update/Sha256.h"

#include <cctype>
#include <cstring>
#include <fstream>
#include <vector>

namespace hsf {
namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline uint32_t RotR(uint32_t value, int bits) { return (value >> bits) | (value << (32 - bits)); }

}  // namespace

void Sha256::Reset() {
  state_[0] = 0x6a09e667u;
  state_[1] = 0xbb67ae85u;
  state_[2] = 0x3c6ef372u;
  state_[3] = 0xa54ff53au;
  state_[4] = 0x510e527fu;
  state_[5] = 0x9b05688cu;
  state_[6] = 0x1f83d9abu;
  state_[7] = 0x5be0cd19u;
  bitCount_ = 0;
  bufferLength_ = 0;
}

void Sha256::Transform(const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = RotR(w[i - 15], 7) ^ RotR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = RotR(w[i - 2], 17) ^ RotR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

  for (int i = 0; i < 64; ++i) {
    const uint32_t S1 = RotR(e, 6) ^ RotR(e, 11) ^ RotR(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t temp1 = h + S1 + ch + kRoundConstants[i] + w[i];
    const uint32_t S0 = RotR(a, 2) ^ RotR(a, 13) ^ RotR(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = S0 + maj;

    h = g; g = f; f = e;
    e = d + temp1;
    d = c; c = b; b = a;
    a = temp1 + temp2;
  }

  state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
  state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::Update(const void* data, size_t length) {
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  bitCount_ += static_cast<uint64_t>(length) * 8;

  while (length > 0) {
    const size_t space = 64 - bufferLength_;
    const size_t take = length < space ? length : space;
    std::memcpy(buffer_ + bufferLength_, bytes, take);
    bufferLength_ += take;
    bytes += take;
    length -= take;
    if (bufferLength_ == 64) {
      Transform(buffer_);
      bufferLength_ = 0;
    }
  }
}

std::string Sha256::HexDigest() {
  const uint64_t totalBits = bitCount_;

  // Padding: 0x80, then zeroes, then the 64-bit big-endian length.
  const uint8_t one = 0x80;
  Update(&one, 1);
  const uint8_t zero = 0x00;
  while (bufferLength_ != 56) Update(&zero, 1);

  uint8_t lengthBytes[8];
  for (int i = 0; i < 8; ++i) lengthBytes[i] = static_cast<uint8_t>((totalBits >> (56 - i * 8)) & 0xFF);
  // Written straight into the buffer: routing it through Update() would add
  // these 8 bytes to bitCount_, which is the value being encoded.
  std::memcpy(buffer_ + bufferLength_, lengthBytes, 8);
  Transform(buffer_);
  bufferLength_ = 0;

  static const char* kHex = "0123456789abcdef";
  std::string hex;
  hex.reserve(64);
  for (int i = 0; i < 8; ++i) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      const uint8_t byte = static_cast<uint8_t>((state_[i] >> shift) & 0xFF);
      hex.push_back(kHex[byte >> 4]);
      hex.push_back(kHex[byte & 0x0F]);
    }
  }
  return hex;
}

std::string Sha256::HexOf(const std::string& data) {
  Sha256 hasher;
  hasher.Update(data.data(), data.size());
  return hasher.HexDigest();
}

std::string Sha256::HexOfFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return std::string();

  Sha256 hasher;
  std::vector<char> chunk(64 * 1024);
  while (in.good()) {
    in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
    const std::streamsize got = in.gcount();
    if (got > 0) hasher.Update(chunk.data(), static_cast<size_t>(got));
  }
  if (in.bad()) return std::string();
  return hasher.HexDigest();
}

bool Sha256::RawOfFile(const std::string& path, uint8_t out[32]) {
  const std::string hex = HexOfFile(path);
  if (hex.size() != 64) return false;
  for (int i = 0; i < 32; ++i) {
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      return (c | 0x20) - 'a' + 10;
    };
    out[i] = static_cast<uint8_t>((nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
  }
  return true;
}

bool Sha256::HexEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char difference = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    difference |= static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])) ^
                                              std::tolower(static_cast<unsigned char>(b[i])));
  }
  return difference == 0;
}

}  // namespace hsf
