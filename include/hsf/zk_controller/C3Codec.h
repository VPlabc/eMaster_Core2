#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hsf::c3 {

// C3/InBio access-control panel wire protocol (TCP port 4370).
//
// WHY THIS EXISTS. The gateway's existing ZKTeco support goes through the
// PullSDK, which is a 32-bit Windows DLL -- so every Linux and 64-bit Windows
// build gets PullSdkClient_stub.cpp and every zk.* call fails with
// NotSupportedOnThisPlatform. C3 is the panel's own TCP protocol, so speaking
// it directly gives those builds real door control and real card events with
// no vendor binary at all.
//
// PROVENANCE. Ported from the Qt implementation in qt-app-base's
// src/features/c3protocol (c3_codec.{h,cpp}), which documents the frame shape
// and CRC parameters as having been learned by reading zkaccess-c3-py (GPL-3.0)
// as documentation of an otherwise-unpublished protocol. The wire format and
// every empirically-discovered constant below are carried across UNCHANGED --
// the port is Qt types to std types, not a redesign. Where that source recorded
// a fact discovered against real hardware, the comment comes with it, because
// those are the parts nobody can re-derive from first principles.
//
// FRAME:
//   0xAA (start) + version(1) + command(1) + length_lsb(1) + length_msb(1)
//   + [session_id_lsb, session_id_msb, request_nr_lsb, request_nr_msb]
//        -- 4 bytes, present on every message once a session exists
//   + payload (`length` counts the 4 session bytes too, when present)
//   + crc16_lsb + crc16_msb + 0x55 (end)
//
// CRC-16/ARC (poly 0xA001, init 0x0000, reflected) over version..payload --
// NOT the start byte, NOT the CRC bytes, NOT the end byte.

using Bytes = std::vector<uint8_t>;

constexpr uint8_t kFrameStart = 0xAA;
constexpr uint8_t kFrameEnd = 0x55;
constexpr uint8_t kProtocolVersion = 0x01;

constexpr uint8_t kCommandConnectSessionLess = 0x01;
constexpr uint8_t kCommandDisconnect = 0x02;
constexpr uint8_t kCommandGetParam = 0x04;
constexpr uint8_t kCommandControl = 0x05;
constexpr uint8_t kCommandDataTableCfg = 0x06;
constexpr uint8_t kCommandGetData = 0x08;
constexpr uint8_t kCommandRtlogBinary = 0x0B;
constexpr uint8_t kCommandConnectSession = 0x76;
constexpr uint8_t kCommandRtlogKeyValue = 0x79;
constexpr uint8_t kReplyOk = 0xC8;
constexpr uint8_t kReplyError = 0xC9;

// Sentinels the panel accepts in the very first CONNECT_SESSION frame, before a
// real session exists. An empirically-discovered protocol fact, not a stylistic
// choice -- do not "clean these up".
constexpr uint16_t kPreSessionId = 0xFEFE;
constexpr int32_t kPreSessionRequestNr = -258;

// CRC-16/ARC. Verify any implementation against crc16("123456789") == 0xBB3D,
// the standard published check value, before trusting any other vector.
uint16_t Crc16(const uint8_t* data, size_t length);
uint16_t Crc16(const Bytes& data);

enum class ResponseStatus {
  kIncomplete,  // fewer bytes than the frame's own length field promises -- wait for more
  kMalformed,   // bad start/end marker, bad CRC, or a command byte that is neither reply
  kRejected,    // well-formed, command byte == kReplyError
  kOk
};

// --- session ----------------------------------------------------------------

// CONNECT_SESSION uses kPreSessionId/kPreSessionRequestNr and DOES include the
// 4-byte session block even though no session exists yet -- the protocol's own
// behaviour. `password` empty means a no-password connect.
Bytes EncodeConnectSession(const std::string& password);

struct ConnectSessionResponse {
  ResponseStatus status = ResponseStatus::kIncomplete;
  uint16_t session_id = 0;  // payload bytes 0-1, little-endian; valid only when kOk
};
ConnectSessionResponse DecodeConnectSession(const Bytes& data);

// CONNECT_SESSION_LESS omits the session block entirely. Confirmed live against
// a real panel that rejects CONNECT_SESSION outright, so this is the path that
// actually works on at least some hardware -- try session first, fall back.
Bytes EncodeConnectSessionLess(const std::string& password);

Bytes EncodeDisconnect(uint16_t sessionId, int32_t requestNr);
Bytes EncodeSessionLessDisconnect();

// A reply carrying no session-ID payload: CONNECT_SESSION_LESS's own reply and
// any DISCONNECT reply, both confirmed to have a zero-length payload on
// success. Distinct from ConnectSessionResponse, which would misread that.
struct GenericReply {
  ResponseStatus status = ResponseStatus::kIncomplete;
};
GenericReply DecodeGenericReply(const Bytes& data);

// --- control (what ZkController drives) --------------------------------------

// Values match zkaccess-c3-py's consts.py exactly.
enum class ControlOperation : uint8_t {
  kOutput = 1,
  kCancelAlarm = 2,
  kRestartDevice = 3,
  kEnableDisableNormalOpen = 4
};
enum class ControlOutputAddress : uint8_t { kDoor = 1, kAux = 2 };

// Payload is [operation, param1, param2, param3, 0 (param4, reserved)].
//   kOutput: param1 = door/aux number, param2 = ControlOutputAddress,
//            param3 = duration (0 = close, 255 = stay open, 1-254 = seconds)
//   kCancelAlarm / kRestartDevice: all params 0
//   kEnableDisableNormalOpen: param1 = door, param2 = 0 disable / 1 enable
Bytes EncodeControl(ControlOperation operation, uint8_t param1, uint8_t param2, uint8_t param3,
                    bool includeSessionBlock, uint16_t sessionId, int32_t requestNr);

// --- realtime log -------------------------------------------------------------

enum class RtLogRecordKind { kEvent, kDoorAlarmStatus };

// One 16-byte binary RT log record. Both kinds share the envelope; only bytes
// 0-11 differ. The offsets follow zkaccess-c3-py's CODE, not its docstring --
// the two disagree, and the code is what real panels match.
struct RtLogRecord {
  RtLogRecordKind kind = RtLogRecordKind::kEvent;
  uint32_t card_no = 0;   // Event only, bytes 0-3 LE
  uint32_t pin = 0;       // Event only, bytes 4-7 LE
  Bytes alarm_status;     // DoorAlarmStatus only, bytes 0-3 raw
  Bytes dss_status;       // DoorAlarmStatus only, bytes 4-7 raw
  uint8_t verified = 0;   // Event: byte 8. DoorAlarmStatus: byte 9 -- NOT byte 8
  uint8_t door_id = 0;    // Event only, byte 9
  uint8_t event_type = 0; // byte 10 both kinds; 255 marks a DoorAlarmStatus record
  uint8_t in_out_state = 0;  // Event only, byte 11
  uint32_t time_second = 0;  // both kinds, bytes 12-15 LE, raw C3 time encoding
};

// The panel's own encoding for the timestamp in time_second. Decoded here
// rather than left raw because every consumer needs a real time.
struct C3DateTime {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
};
C3DateTime DecodeC3Time(uint32_t value);
std::string FormatC3Time(uint32_t value);  // "YYYY-MM-DD HH:MM:SS"

Bytes EncodeRtlogBinaryRequest(bool includeSessionBlock, uint16_t sessionId, int32_t requestNr);
Bytes EncodeRtlogKeyValueRequest(bool includeSessionBlock, uint16_t sessionId, int32_t requestNr);

enum class RtLogStatus { kIncomplete, kMalformed, kRejected, kNotBinaryMode, kOk };

struct RtLogBinaryResponse {
  RtLogStatus status = RtLogStatus::kIncomplete;
  std::vector<RtLogRecord> records;
};
// kNotBinaryMode means the payload length is not a multiple of 16 -- the
// panel's documented way of saying it does not support binary RT log. The
// caller should switch to the key/value command and stay switched.
RtLogBinaryResponse DecodeRtlogBinary(const Bytes& data, bool includeSessionBlock);

struct RtLogKeyValueResponse {
  ResponseStatus status = ResponseStatus::kIncomplete;
  std::vector<nlohmann::json> records;  // 0 or 1, string-valued, keys un-interpreted
};
RtLogKeyValueResponse DecodeRtlogKeyValue(const Bytes& data, bool includeSessionBlock);

// --- parameters ----------------------------------------------------------------

// Payload is the requested names comma-separated, nothing else.
Bytes EncodeGetParam(const std::vector<std::string>& names, bool includeSessionBlock,
                     uint16_t sessionId, int32_t requestNr);

struct GetParamResponse {
  ResponseStatus status = ResponseStatus::kIncomplete;
  nlohmann::json values = nlohmann::json::object();  // only what the panel returned
};
GetParamResponse DecodeGetParam(const Bytes& data, bool includeSessionBlock);

// --- data tables ------------------------------------------------------------------

struct DataTableField {
  std::string name;
  // 'i' little-endian unsigned int, 's' ASCII string, 'L' also a little-endian
  // unsigned int ("long") decoded identically to 'i' -- seen live on real
  // hardware for wide fields such as a full card number.
  char type = 'i';
  int index = 0;
};

struct DataTableConfig {
  std::string name;
  int index = 0;
  std::vector<DataTableField> fields;
};

struct DataTableConfigResponse {
  ResponseStatus status = ResponseStatus::kIncomplete;
  std::vector<DataTableConfig> tables;
};

Bytes EncodeDataTableConfig(bool includeSessionBlock, uint16_t sessionId, int32_t requestNr);
DataTableConfigResponse DecodeDataTableConfig(const Bytes& data, bool includeSessionBlock);

// Payload: tableIndex, fieldCount, each fieldIndex, then two reserved zero
// bytes whose purpose is undocumented even in the reference project. Carried
// through unchanged rather than dropped.
Bytes EncodeGetData(int tableIndex, const std::vector<int>& fieldIndexes, bool includeSessionBlock,
                    uint16_t sessionId, int32_t requestNr);

enum class GetDataStatus { kIncomplete, kMalformed, kRejected, kTableMismatch, kOk };

struct GetDataResponse {
  GetDataStatus status = GetDataStatus::kIncomplete;
  std::vector<nlohmann::json> records;
};
GetDataResponse DecodeGetData(const Bytes& data, int expectedTableIndex,
                              const std::vector<DataTableField>& tableFields,
                              bool includeSessionBlock);

// --- shared helpers -----------------------------------------------------------------

// "key=value,key=value" text, the reply shape DATATABLE_CFG, GETPARAM and
// RTLOG_KEYVALUE all use.
std::vector<std::pair<std::string, std::string>> ParseKeyValuePairs(const uint8_t* data,
                                                                     size_t length);

// How many bytes the frame at the front of `data` claims to be, or 0 when there
// is not yet enough to tell. Lets a reader consume exactly one frame from a
// stream buffer without re-decoding it.
size_t FrameSize(const Bytes& data);

}  // namespace hsf::c3
