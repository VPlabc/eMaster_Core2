#pragma once

#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hsf/ModbusCodec.h"

namespace hsf {

// Which Modbus address space an input point lives in. Coils (FC01) and
// discrete inputs (FC02) are separate spaces, and plenty of PLCs answer BOTH
// function codes at a given address while only one of them carries the real
// value -- the other returns a constant. That cannot be detected by probing
// (both "work"), so the point has to say which space it means.
enum class ModbusSource {
  kAuto,             // FC02, falling back to FC01 if the PLC rejects it
  kDiscreteInput,    // force FC02
  kCoil              // force FC01
};

const char* ToString(ModbusSource source);
ModbusSource ParseModbusSource(const std::string& text, ModbusSource fallback = ModbusSource::kAuto);

struct ModbusPoint {
  std::string name;
  int address = 0;
  // Which Lua runtime registered this point (LuaEngine::RuntimeId()), so
  // stopping or re-running one script clears only its own dashboard I/O.
  // Before several scripts could run at once, a restart cleared everything,
  // which was indistinguishable from correct.
  int64_t owner = 0;
  bool value = false;
  // False until the address has been read back from the PLC at least once,
  // so the dashboard can show "unknown" rather than a confident OFF for a
  // point that has never actually been polled.
  bool valid = false;

  // Inputs only; outputs are always coils (FC01), the only writable bit space.
  ModbusSource source = ModbusSource::kAuto;
  // Function code that actually produced `value` (0 until first read). Shown
  // on the dashboard so an operator can see which space answered without
  // reading the script -- the whole failure mode here is invisible otherwise.
  int function_code = 0;
};

// A named 16-bit register range, decoded as a typed value.
//
// Holding registers (FC03) are the read/WRITE space; input registers (FC04)
// are read-only -- Modbus defines no FC04 write, because that space models
// values the device produces rather than values you hand it. `writable` is
// therefore never true for an input register, regardless of what a script
// asks for.
struct ModbusRegisterPoint {
  std::string name;
  int address = 0;
  int64_t owner = 0;  // see ModbusPoint::owner
  RegisterFormat format;
  bool input_register = false;  // true => FC04 (read-only), false => FC03
  bool writable = false;        // holding registers only
  std::string unit;             // display only, e.g. "degC"

  // Raw words exactly as read, so the dashboard can show the underlying
  // registers next to the decoded value -- indispensable when a value looks
  // wrong and the question is "is it the data or my endianness?".
  std::vector<uint16_t> raw;
  nlohmann::json value;
  bool valid = false;
  std::string error;
};

// Names the Lua script attaches to Modbus coils, so the dashboard can label
// PLC I/O instead of showing bare addresses -- see
// request/updateUI.md section 8:
//
//   Modbus.RegisterInput("CardTaken", 1024)
//   Modbus.RegisterOutput("CardScan", 1280)
//
// The gateway polls whatever is registered here and pushes the values over
// the WebSocket. Registrations are wiped whenever a script starts (see
// LuaEngine::RunSource) -- they describe the running script, so leaving the
// previous script's points on the dashboard would show I/O nothing is
// driving any more.
//
// Toggling from the dashboard resolves an output BY NAME through this
// registry rather than taking an address from the request. That keeps the
// endpoint from being a general "write any coil" primitive: only points a
// script deliberately registered can be driven.
class ModbusRegistry {
 public:
  static ModbusRegistry& Instance();

  // Re-registering an existing name updates its address rather than adding
  // a duplicate row; a script re-run that registers the same names must not
  // grow the list.
  void RegisterInput(const std::string& name, int address, ModbusSource source = ModbusSource::kAuto,
                      int64_t owner = 0);
  void RegisterOutput(const std::string& name, int address, int64_t owner = 0);
  void RegisterRegister(const ModbusRegisterPoint& point);
  void Clear();

  // Drops only the points registered by one Lua runtime. This is what a
  // script restart or stop uses, so the other scripts' named I/O survives.
  void ClearOwner(int64_t owner);

  std::vector<ModbusPoint> Inputs() const;
  std::vector<ModbusPoint> Outputs() const;
  std::vector<ModbusRegisterPoint> Registers() const;
  bool Empty() const;

  // Stores a successful read (raw words + decoded value) or the failure that
  // stopped it, keyed by name.
  void UpdateRegister(const std::string& name, const std::vector<uint16_t>& raw, const nlohmann::json& value);
  void UpdateRegisterError(const std::string& name, const std::string& error);

  // Returns false when `name` isn't a registered register.
  bool FindRegister(const std::string& name, ModbusRegisterPoint& out) const;

  // Keyed by NAME, not address: two points may share an address while
  // reading from different spaces (a deliberate way to compare FC01 against
  // FC02 on one terminal), and an address-keyed update would let each one
  // clobber the other's value and function code.
  // `functionCode` records which space answered, for the dashboard.
  void UpdateInput(const std::string& name, bool value, int functionCode);
  void UpdateOutput(const std::string& name, bool value);

  // Latches an kAuto point onto the space that actually answered, so the
  // poller stops paying for a failed FC02 on every cycle.
  void SetResolvedSource(const std::string& name, ModbusSource source);

  // Returns false when `name` isn't a registered output.
  bool FindOutputAddress(const std::string& name, int& address) const;

  nlohmann::json ToJson() const;

 private:
  ModbusRegistry() = default;
  ModbusRegistry(const ModbusRegistry&) = delete;
  ModbusRegistry& operator=(const ModbusRegistry&) = delete;

  static void UpsertUnlocked(std::vector<ModbusPoint>& points, const std::string& name, int address,
                              ModbusSource source, int64_t owner);
  static void UpdateUnlocked(std::vector<ModbusPoint>& points, const std::string& name, bool value,
                              int functionCode);
  static nlohmann::json PointsToJson(const std::vector<ModbusPoint>& points);

  mutable std::mutex mutex_;
  std::vector<ModbusPoint> inputs_;
  std::vector<ModbusPoint> outputs_;
  std::vector<ModbusRegisterPoint> registers_;
};

}  // namespace hsf
