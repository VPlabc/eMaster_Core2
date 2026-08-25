#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hsf {

// Value types that can be packed into a run of 16-bit Modbus registers.
// Modbus itself has no notion of these -- it moves 16-bit words and nothing
// else -- so the width and byte order of anything larger is a convention
// between the PLC program and whoever reads it. That convention is exactly
// what this file makes explicit.
enum class RegisterType {
  kUInt16,
  kInt16,
  kUInt32,
  kInt32,
  kUInt64,
  kInt64,
  kFloat32,
  kFloat64,
  kString,
};

// How the bytes of a multi-register value are ordered on the wire. PLC
// documentation almost always names these by the four-letter pattern, so
// those spellings are accepted directly:
//
//   ABCD  big endian            no swaps
//   BADC  byte-swapped          bytes swapped inside each 16-bit word
//   CDAB  word-swapped          16-bit words in reverse order ("mid-little")
//   DCBA  little endian         both
//
// A register always travels big-endian on the wire; these flags describe how
// the PLC laid the value across registers, not the framing.
struct RegisterFormat {
  RegisterType type = RegisterType::kUInt16;
  bool byte_swap = false;  // swap the two bytes within each 16-bit word
  bool word_swap = false;  // reverse the order of the 16-bit words
  int length = 0;          // kString only: length in BYTES (not registers)
};

const char* ToString(RegisterType type);
// Returns false when `text` names no known type.
bool ParseRegisterType(const std::string& text, RegisterType& out);

// Accepts "abcd"/"badc"/"cdab"/"dcba", plus "big"/"little" and the more
// verbose "big_endian_byte_swap" style spellings. Returns false if unknown.
bool ParseEndian(const std::string& text, bool& byteSwap, bool& wordSwap);
// The canonical four-letter name for the current flags.
const char* EndianName(bool byteSwap, bool wordSwap);

// Number of 16-bit registers a value of this format occupies.
int RegisterCount(const RegisterFormat& format);

// registers -> typed value. `out` receives a JSON number or string.
bool DecodeRegisters(const RegisterFormat& format, const std::vector<uint16_t>& registers,
                      nlohmann::json& out, std::string& error);

// typed value -> registers, for writing back. Rejects values that do not fit
// the target type rather than silently truncating -- a wrapped setpoint
// written to a machine is worse than a refused one.
bool EncodeRegisters(const RegisterFormat& format, const nlohmann::json& value,
                      std::vector<uint16_t>& out, std::string& error);

}  // namespace hsf
