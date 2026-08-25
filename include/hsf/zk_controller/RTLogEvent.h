#pragma once

#include <cstdint>
#include <string>

namespace hsf {

// Ported verbatim (field-for-field) from
// ZKTecoProtocol/src/rtlog/rtlog_event.h -- inOutStatus is intentionally
// NOT renamed to/exposed as "ReaderID": the source project's RTLogEvent has
// no dedicated reader-index field, and the field's real meaning must be
// confirmed against actual controller output (see
// request/HSF_Machine_ZK_Controller_Lua_Integration.md, "Important: Verify
// ReaderID") before anything maps a Lua ReaderID onto it.
struct RTLogEvent {
  std::string time;
  std::string pin;
  std::string cardNo;
  int doorNo = 0;
  int eventType = 0;
  int inOutStatus = 0;
  int verifyMode = 0;
};

struct DoorAlarmStatus {
  std::string time;
  uint32_t dssStatusPacked = 0;
  uint32_t alarmStatusPacked = 0;
};

}  // namespace hsf
