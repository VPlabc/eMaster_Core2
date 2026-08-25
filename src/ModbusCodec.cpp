#include "hsf/ModbusCodec.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace hsf {

using nlohmann::json;

namespace {

std::string Lower(const std::string& text) {
  std::string out = text;
  std::transform(out.begin(), out.end(), out.begin(),
                  [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

// Registers -> flat byte buffer, applying the two swap flags. Every decode
// and encode below works on this buffer in plain big-endian order, so the
// endian handling lives in exactly one place.
std::vector<uint8_t> ToBytes(const std::vector<uint16_t>& registers, bool byteSwap, bool wordSwap) {
  std::vector<uint16_t> words = registers;
  if (wordSwap) std::reverse(words.begin(), words.end());

  std::vector<uint8_t> bytes;
  bytes.reserve(words.size() * 2);
  for (uint16_t word : words) {
    uint8_t hi = static_cast<uint8_t>((word >> 8) & 0xFF);
    uint8_t lo = static_cast<uint8_t>(word & 0xFF);
    if (byteSwap) {
      bytes.push_back(lo);
      bytes.push_back(hi);
    } else {
      bytes.push_back(hi);
      bytes.push_back(lo);
    }
  }
  return bytes;
}

// Inverse of ToBytes.
std::vector<uint16_t> FromBytes(const std::vector<uint8_t>& bytes, bool byteSwap, bool wordSwap) {
  std::vector<uint16_t> words;
  words.reserve(bytes.size() / 2);
  for (size_t i = 0; i + 1 < bytes.size(); i += 2) {
    uint8_t first = bytes[i];
    uint8_t second = bytes[i + 1];
    if (byteSwap) std::swap(first, second);
    words.push_back(static_cast<uint16_t>((static_cast<uint16_t>(first) << 8) | second));
  }
  if (wordSwap) std::reverse(words.begin(), words.end());
  return words;
}

uint64_t ReadBigEndian(const std::vector<uint8_t>& bytes, size_t count) {
  uint64_t value = 0;
  for (size_t i = 0; i < count && i < bytes.size(); ++i) {
    value = (value << 8) | bytes[i];
  }
  return value;
}

void WriteBigEndian(std::vector<uint8_t>& bytes, uint64_t value, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    bytes.push_back(static_cast<uint8_t>((value >> (8 * (count - 1 - i))) & 0xFF));
  }
}

}  // namespace

const char* ToString(RegisterType type) {
  switch (type) {
    case RegisterType::kInt16: return "int16";
    case RegisterType::kUInt32: return "uint32";
    case RegisterType::kInt32: return "int32";
    case RegisterType::kUInt64: return "uint64";
    case RegisterType::kInt64: return "int64";
    case RegisterType::kFloat32: return "float32";
    case RegisterType::kFloat64: return "float64";
    case RegisterType::kString: return "string";
    default: return "uint16";
  }
}

bool ParseRegisterType(const std::string& text, RegisterType& out) {
  const std::string t = Lower(text);
  // Several spellings per type: PLC tooling variously calls these "word",
  // "dword", "real", "float"... rejecting all but one name would just be an
  // obstacle.
  if (t == "uint16" || t == "u16" || t == "word" || t == "ushort") { out = RegisterType::kUInt16; return true; }
  if (t == "int16" || t == "i16" || t == "s16" || t == "short") { out = RegisterType::kInt16; return true; }
  if (t == "uint32" || t == "u32" || t == "dword" || t == "udint") { out = RegisterType::kUInt32; return true; }
  if (t == "int32" || t == "i32" || t == "s32" || t == "dint") { out = RegisterType::kInt32; return true; }
  if (t == "uint64" || t == "u64" || t == "lword" || t == "ulint") { out = RegisterType::kUInt64; return true; }
  if (t == "int64" || t == "i64" || t == "s64" || t == "lint") { out = RegisterType::kInt64; return true; }
  if (t == "float32" || t == "f32" || t == "float" || t == "real") { out = RegisterType::kFloat32; return true; }
  if (t == "float64" || t == "f64" || t == "double" || t == "lreal") { out = RegisterType::kFloat64; return true; }
  if (t == "string" || t == "str" || t == "ascii" || t == "char") { out = RegisterType::kString; return true; }
  return false;
}

bool ParseEndian(const std::string& text, bool& byteSwap, bool& wordSwap) {
  const std::string t = Lower(text);
  if (t == "abcd" || t == "big" || t == "big_endian" || t == "be") {
    byteSwap = false; wordSwap = false; return true;
  }
  if (t == "badc" || t == "big_byte_swap" || t == "big_endian_byte_swap" || t == "be_bs") {
    byteSwap = true; wordSwap = false; return true;
  }
  if (t == "cdab" || t == "little_byte_swap" || t == "little_endian_byte_swap" || t == "word_swap" ||
      t == "mid_little" || t == "le_bs") {
    byteSwap = false; wordSwap = true; return true;
  }
  if (t == "dcba" || t == "little" || t == "little_endian" || t == "le") {
    byteSwap = true; wordSwap = true; return true;
  }
  return false;
}

const char* EndianName(bool byteSwap, bool wordSwap) {
  if (!byteSwap && !wordSwap) return "ABCD";
  if (byteSwap && !wordSwap) return "BADC";
  if (!byteSwap && wordSwap) return "CDAB";
  return "DCBA";
}

int RegisterCount(const RegisterFormat& format) {
  switch (format.type) {
    case RegisterType::kUInt16:
    case RegisterType::kInt16:
      return 1;
    case RegisterType::kUInt32:
    case RegisterType::kInt32:
    case RegisterType::kFloat32:
      return 2;
    case RegisterType::kUInt64:
    case RegisterType::kInt64:
    case RegisterType::kFloat64:
      return 4;
    case RegisterType::kString:
      // Two bytes per register, rounded up; at least one.
      return std::max(1, (std::max(0, format.length) + 1) / 2);
  }
  return 1;
}

bool DecodeRegisters(const RegisterFormat& format, const std::vector<uint16_t>& registers, json& out,
                      std::string& error) {
  const int needed = RegisterCount(format);
  if (static_cast<int>(registers.size()) < needed) {
    error = "expected " + std::to_string(needed) + " register(s), got " + std::to_string(registers.size());
    return false;
  }

  std::vector<uint16_t> slice(registers.begin(), registers.begin() + needed);
  std::vector<uint8_t> bytes = ToBytes(slice, format.byte_swap, format.word_swap);

  switch (format.type) {
    case RegisterType::kUInt16:
      out = static_cast<uint16_t>(ReadBigEndian(bytes, 2));
      return true;
    case RegisterType::kInt16:
      out = static_cast<int16_t>(static_cast<uint16_t>(ReadBigEndian(bytes, 2)));
      return true;
    case RegisterType::kUInt32:
      out = static_cast<uint32_t>(ReadBigEndian(bytes, 4));
      return true;
    case RegisterType::kInt32:
      out = static_cast<int32_t>(static_cast<uint32_t>(ReadBigEndian(bytes, 4)));
      return true;
    case RegisterType::kUInt64:
      out = ReadBigEndian(bytes, 8);
      return true;
    case RegisterType::kInt64:
      out = static_cast<int64_t>(ReadBigEndian(bytes, 8));
      return true;
    case RegisterType::kFloat32: {
      uint32_t raw = static_cast<uint32_t>(ReadBigEndian(bytes, 4));
      float value = 0.0f;
      std::memcpy(&value, &raw, sizeof(value));
      // JSON has no NaN/Inf. Emitting one produces literal `NaN` in the
      // payload, which is invalid JSON and breaks the dashboard's parse for
      // every other point too -- so report it as null instead.
      if (!std::isfinite(value)) {
        out = nullptr;
        return true;
      }
      out = value;
      return true;
    }
    case RegisterType::kFloat64: {
      uint64_t raw = ReadBigEndian(bytes, 8);
      double value = 0.0;
      std::memcpy(&value, &raw, sizeof(value));
      if (!std::isfinite(value)) {
        out = nullptr;
        return true;
      }
      out = value;
      return true;
    }
    case RegisterType::kString: {
      std::string text;
      const size_t limit = std::min(bytes.size(), static_cast<size_t>(std::max(0, format.length)));
      for (size_t i = 0; i < limit; ++i) {
        if (bytes[i] == 0) break;  // NUL terminates, as in every PLC string convention
        text.push_back(static_cast<char>(bytes[i]));
      }
      // Fixed-width PLC strings are space padded.
      while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
      out = text;
      return true;
    }
  }

  error = "unsupported register type";
  return false;
}

bool EncodeRegisters(const RegisterFormat& format, const json& value, std::vector<uint16_t>& out,
                      std::string& error) {
  std::vector<uint8_t> bytes;

  // Range-checks an integral value against the target width before it is
  // truncated into the buffer.
  auto checkSigned = [&](long long v, long long lo, long long hi) {
    if (v < lo || v > hi) {
      error = "value " + std::to_string(v) + " does not fit " + ToString(format.type);
      return false;
    }
    return true;
  };
  auto checkUnsigned = [&](unsigned long long v, unsigned long long hi) {
    if (v > hi) {
      error = "value " + std::to_string(v) + " does not fit " + ToString(format.type);
      return false;
    }
    return true;
  };

  if (format.type == RegisterType::kString) {
    if (!value.is_string()) {
      error = "expected a string value";
      return false;
    }
    std::string text = value.get<std::string>();
    const size_t width = static_cast<size_t>(RegisterCount(format)) * 2;
    if (text.size() > width) {
      error = "string is " + std::to_string(text.size()) + " bytes, field holds " + std::to_string(width);
      return false;
    }
    bytes.assign(text.begin(), text.end());
    bytes.resize(width, 0);  // NUL pad to the full field
    out = FromBytes(bytes, format.byte_swap, format.word_swap);
    return true;
  }

  if (format.type == RegisterType::kFloat32 || format.type == RegisterType::kFloat64) {
    if (!value.is_number()) {
      error = "expected a numeric value";
      return false;
    }
    double d = value.get<double>();
    if (format.type == RegisterType::kFloat32) {
      float f = static_cast<float>(d);
      uint32_t raw = 0;
      std::memcpy(&raw, &f, sizeof(raw));
      WriteBigEndian(bytes, raw, 4);
    } else {
      uint64_t raw = 0;
      std::memcpy(&raw, &d, sizeof(raw));
      WriteBigEndian(bytes, raw, 8);
    }
    out = FromBytes(bytes, format.byte_swap, format.word_swap);
    return true;
  }

  if (!value.is_number()) {
    error = "expected a numeric value";
    return false;
  }
  // Reject a fractional value for an integer register rather than rounding it
  // silently -- 12.7 written to an int16 setpoint should be an error, not 12.
  if (value.is_number_float()) {
    double d = value.get<double>();
    if (d != std::floor(d)) {
      error = "expected a whole number for " + std::string(ToString(format.type));
      return false;
    }
  }

  switch (format.type) {
    case RegisterType::kUInt16: {
      auto v = value.get<long long>();
      if (v < 0 || !checkUnsigned(static_cast<unsigned long long>(v), 0xFFFFULL)) {
        if (v < 0) error = "value " + std::to_string(v) + " does not fit uint16";
        return false;
      }
      WriteBigEndian(bytes, static_cast<uint64_t>(v), 2);
      break;
    }
    case RegisterType::kInt16: {
      auto v = value.get<long long>();
      if (!checkSigned(v, -32768, 32767)) return false;
      WriteBigEndian(bytes, static_cast<uint64_t>(static_cast<uint16_t>(static_cast<int16_t>(v))), 2);
      break;
    }
    case RegisterType::kUInt32: {
      auto v = value.get<long long>();
      if (v < 0 || !checkUnsigned(static_cast<unsigned long long>(v), 0xFFFFFFFFULL)) {
        if (v < 0) error = "value " + std::to_string(v) + " does not fit uint32";
        return false;
      }
      WriteBigEndian(bytes, static_cast<uint64_t>(v), 4);
      break;
    }
    case RegisterType::kInt32: {
      auto v = value.get<long long>();
      if (!checkSigned(v, -2147483648LL, 2147483647LL)) return false;
      WriteBigEndian(bytes, static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(v))), 4);
      break;
    }
    case RegisterType::kUInt64: {
      auto v = value.get<unsigned long long>();
      WriteBigEndian(bytes, static_cast<uint64_t>(v), 8);
      break;
    }
    case RegisterType::kInt64: {
      auto v = value.get<long long>();
      WriteBigEndian(bytes, static_cast<uint64_t>(v), 8);
      break;
    }
    default:
      error = "unsupported register type";
      return false;
  }

  out = FromBytes(bytes, format.byte_swap, format.word_swap);
  return true;
}

}  // namespace hsf
