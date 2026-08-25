#include "hsf/zk_controller/C3Codec.h"

#include <cstdio>
#include <cstring>

namespace hsf::c3 {
namespace {

void AppendU8(Bytes& out, uint8_t value) { out.push_back(value); }

void AppendU16(Bytes& out, uint16_t value) {
  out.push_back(static_cast<uint8_t>(value & 0xFF));
  out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

uint16_t ReadU16(const Bytes& data, size_t offset) {
  return static_cast<uint16_t>(data[offset]) | static_cast<uint16_t>(data[offset + 1]) << 8;
}

uint32_t ReadU32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | static_cast<uint32_t>(data[1]) << 8 |
         static_cast<uint32_t>(data[2]) << 16 | static_cast<uint32_t>(data[3]) << 24;
}

Bytes EncodeFrame(uint8_t command, uint16_t sessionId, int32_t requestNr, const Bytes& payload,
                  bool includeSessionBlock = true) {
  Bytes body;
  AppendU8(body, kProtocolVersion);
  AppendU8(body, command);
  // `length` counts the 4 session bytes as part of the payload when present.
  AppendU16(body, static_cast<uint16_t>((includeSessionBlock ? 4 : 0) + payload.size()));
  if (includeSessionBlock) {
    AppendU16(body, sessionId);
    // The request number is signed on the wire but transmitted as 16 bits;
    // kPreSessionRequestNr is negative, so this truncation is deliberate.
    AppendU16(body, static_cast<uint16_t>(requestNr));
  }
  body.insert(body.end(), payload.begin(), payload.end());

  const uint16_t checksum = Crc16(body);

  Bytes frame;
  frame.reserve(body.size() + 4);
  AppendU8(frame, kFrameStart);
  frame.insert(frame.end(), body.begin(), body.end());
  AppendU16(frame, checksum);
  AppendU8(frame, kFrameEnd);
  return frame;
}

struct DecodedFrame {
  ResponseStatus status = ResponseStatus::kIncomplete;
  Bytes payload;
};

// One decoder for every reply shape. Every length is checked against the actual
// buffer before it is used: this parses bytes straight off a socket, and the
// length field is attacker-controlled in the sense that a confused or hostile
// panel can claim anything.
DecodedFrame DecodeFrameGeneric(const Bytes& data, bool stripSessionBlock) {
  DecodedFrame result;
  if (data.size() < 5) return result;

  const uint16_t length = ReadU16(data, 3);
  const size_t frameSize = 5 + static_cast<size_t>(length) + 2 + 1;
  if (data.size() < frameSize) return result;  // still kIncomplete

  const size_t crcOffset = 5 + static_cast<size_t>(length);
  const uint16_t expectedCrc = ReadU16(data, crcOffset);
  const uint8_t command = data[2];

  if (data[0] != kFrameStart || data[frameSize - 1] != kFrameEnd ||
      Crc16(data.data() + 1, 4 + static_cast<size_t>(length)) != expectedCrc ||
      (command != kReplyOk && command != kReplyError)) {
    result.status = ResponseStatus::kMalformed;
    return result;
  }
  if (command == kReplyError) {
    result.status = ResponseStatus::kRejected;
    return result;
  }

  Bytes payload(data.begin() + 5, data.begin() + 5 + length);
  // Once a session exists the 4-byte block is on EVERY message, not just the
  // connect reply, so it has to come off before the payload means anything.
  if (stripSessionBlock && payload.size() >= 4) payload.erase(payload.begin(), payload.begin() + 4);
  result.status = ResponseStatus::kOk;
  result.payload = std::move(payload);
  return result;
}

Bytes FromString(const std::string& text) { return Bytes(text.begin(), text.end()); }

}  // namespace

uint16_t Crc16(const uint8_t* data, size_t length) {
  uint16_t crc = 0x0000;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U ? static_cast<uint16_t>((crc >> 1) ^ 0xA001U)
                             : static_cast<uint16_t>(crc >> 1);
    }
  }
  return crc;
}

uint16_t Crc16(const Bytes& data) { return Crc16(data.data(), data.size()); }

size_t FrameSize(const Bytes& data) {
  if (data.size() < 5) return 0;
  const uint16_t length = ReadU16(data, 3);
  return 5 + static_cast<size_t>(length) + 2 + 1;
}

// --- session ------------------------------------------------------------------

Bytes EncodeConnectSession(const std::string& password) {
  return EncodeFrame(kCommandConnectSession, kPreSessionId, kPreSessionRequestNr,
                     FromString(password));
}

ConnectSessionResponse DecodeConnectSession(const Bytes& data) {
  ConnectSessionResponse response;
  // Deliberately NOT DecodeFrameGeneric: this reply's payload is the session id
  // itself and must not have a session block stripped off it.
  if (data.size() < 5) return response;

  const uint16_t length = ReadU16(data, 3);
  const size_t frameSize = 5 + static_cast<size_t>(length) + 2 + 1;
  if (data.size() < frameSize) return response;

  const uint16_t expectedCrc = ReadU16(data, 5 + static_cast<size_t>(length));
  const uint8_t command = data[2];
  if (data[0] != kFrameStart || data[frameSize - 1] != kFrameEnd ||
      Crc16(data.data() + 1, 4 + static_cast<size_t>(length)) != expectedCrc ||
      (command != kReplyOk && command != kReplyError)) {
    response.status = ResponseStatus::kMalformed;
    return response;
  }
  if (command == kReplyError) {
    response.status = ResponseStatus::kRejected;
    return response;
  }
  if (length < 2) {
    response.status = ResponseStatus::kMalformed;
    return response;
  }
  response.status = ResponseStatus::kOk;
  response.session_id = ReadU16(data, 5);
  return response;
}

Bytes EncodeConnectSessionLess(const std::string& password) {
  return EncodeFrame(kCommandConnectSessionLess, 0, 0, FromString(password), false);
}

Bytes EncodeDisconnect(uint16_t sessionId, int32_t requestNr) {
  return EncodeFrame(kCommandDisconnect, sessionId, requestNr, {});
}

Bytes EncodeSessionLessDisconnect() { return EncodeFrame(kCommandDisconnect, 0, 0, {}, false); }

GenericReply DecodeGenericReply(const Bytes& data) {
  GenericReply reply;
  // stripSessionBlock false: this shape is used for replies with a zero-length
  // payload, and stripping would be meaningless.
  const DecodedFrame frame = DecodeFrameGeneric(data, false);
  reply.status = frame.status;
  return reply;
}

// --- control --------------------------------------------------------------------

Bytes EncodeControl(ControlOperation operation, uint8_t param1, uint8_t param2, uint8_t param3,
                    bool includeSessionBlock, uint16_t sessionId, int32_t requestNr) {
  Bytes payload;
  AppendU8(payload, static_cast<uint8_t>(operation));
  AppendU8(payload, param1);
  AppendU8(payload, param2);
  AppendU8(payload, param3);
  AppendU8(payload, 0);  // param4, reserved
  return EncodeFrame(kCommandControl, sessionId, requestNr, payload, includeSessionBlock);
}

// --- realtime log --------------------------------------------------------------------

Bytes EncodeRtlogBinaryRequest(bool includeSessionBlock, uint16_t sessionId, int32_t requestNr) {
  return EncodeFrame(kCommandRtlogBinary, sessionId, requestNr, {}, includeSessionBlock);
}

Bytes EncodeRtlogKeyValueRequest(bool includeSessionBlock, uint16_t sessionId, int32_t requestNr) {
  return EncodeFrame(kCommandRtlogKeyValue, sessionId, requestNr, {}, includeSessionBlock);
}

C3DateTime DecodeC3Time(uint32_t value) {
  // ZKTeco's packed seconds-since-2000 encoding, the same one RTLogParser
  // already implements for the PullSDK path -- kept identical so both backends
  // report the same instant for the same event.
  C3DateTime out;
  out.second = static_cast<int>(value % 60);
  value /= 60;
  out.minute = static_cast<int>(value % 60);
  value /= 60;
  out.hour = static_cast<int>(value % 24);
  value /= 24;
  out.day = static_cast<int>(value % 31) + 1;
  value /= 31;
  out.month = static_cast<int>(value % 12) + 1;
  value /= 12;
  out.year = static_cast<int>(value) + 2000;
  return out;
}

std::string FormatC3Time(uint32_t value) {
  const C3DateTime t = DecodeC3Time(value);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", t.year, t.month, t.day,
                t.hour, t.minute, t.second);
  return buffer;
}

RtLogBinaryResponse DecodeRtlogBinary(const Bytes& data, bool includeSessionBlock) {
  RtLogBinaryResponse response;
  const DecodedFrame frame = DecodeFrameGeneric(data, includeSessionBlock);
  switch (frame.status) {
    case ResponseStatus::kIncomplete: return response;
    case ResponseStatus::kMalformed:
      response.status = RtLogStatus::kMalformed;
      return response;
    case ResponseStatus::kRejected:
      response.status = RtLogStatus::kRejected;
      return response;
    case ResponseStatus::kOk: break;
  }

  if (frame.payload.size() % 16 != 0) {
    // The panel's documented way of saying it has no binary RT log mode.
    response.status = RtLogStatus::kNotBinaryMode;
    return response;
  }

  for (size_t offset = 0; offset < frame.payload.size(); offset += 16) {
    const uint8_t* rec = frame.payload.data() + offset;
    RtLogRecord record;
    record.event_type = rec[10];
    if (record.event_type == 255) {
      record.kind = RtLogRecordKind::kDoorAlarmStatus;
      record.alarm_status.assign(rec, rec + 4);
      record.dss_status.assign(rec + 4, rec + 8);
      // Byte 9, not byte 8 -- the two record kinds genuinely differ here.
      record.verified = rec[9];
    } else {
      record.kind = RtLogRecordKind::kEvent;
      record.card_no = ReadU32(rec);
      record.pin = ReadU32(rec + 4);
      record.verified = rec[8];
      record.door_id = rec[9];
      record.in_out_state = rec[11];
    }
    record.time_second = ReadU32(rec + 12);
    response.records.push_back(std::move(record));
  }
  response.status = RtLogStatus::kOk;
  return response;
}

RtLogKeyValueResponse DecodeRtlogKeyValue(const Bytes& data, bool includeSessionBlock) {
  RtLogKeyValueResponse response;
  const DecodedFrame frame = DecodeFrameGeneric(data, includeSessionBlock);
  response.status = frame.status;
  if (frame.status != ResponseStatus::kOk || frame.payload.empty()) return response;

  const auto pairs = ParseKeyValuePairs(frame.payload.data(), frame.payload.size());
  if (pairs.empty()) return response;
  nlohmann::json record = nlohmann::json::object();
  for (const auto& [key, value] : pairs) record[key] = value;
  response.records.push_back(std::move(record));
  return response;
}

// --- parameters -----------------------------------------------------------------------

Bytes EncodeGetParam(const std::vector<std::string>& names, bool includeSessionBlock,
                     uint16_t sessionId, int32_t requestNr) {
  std::string joined;
  for (size_t i = 0; i < names.size(); ++i) {
    if (i > 0) joined += ',';
    joined += names[i];
  }
  return EncodeFrame(kCommandGetParam, sessionId, requestNr, FromString(joined),
                     includeSessionBlock);
}

GetParamResponse DecodeGetParam(const Bytes& data, bool includeSessionBlock) {
  GetParamResponse response;
  const DecodedFrame frame = DecodeFrameGeneric(data, includeSessionBlock);
  response.status = frame.status;
  if (frame.status != ResponseStatus::kOk || frame.payload.empty()) return response;
  for (const auto& [key, value] : ParseKeyValuePairs(frame.payload.data(), frame.payload.size())) {
    response.values[key] = value;
  }
  return response;
}

// --- key/value text ---------------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> ParseKeyValuePairs(const uint8_t* data,
                                                                     size_t length) {
  // Hand-rolled rather than std::regex, which the Qt original used. std::regex
  // is notoriously slow to construct and this runs on every RT log poll on a
  // small board; the grammar is "[\w~]+=[^,\t]+" and a scanner for it is a
  // dozen lines.
  std::vector<std::pair<std::string, std::string>> pairs;
  size_t i = 0;
  while (i < length) {
    // Key: word characters or '~'.
    const size_t keyStart = i;
    while (i < length) {
      const uint8_t c = data[i];
      const bool wordChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_' || c == '~';
      if (!wordChar) break;
      ++i;
    }
    if (i == keyStart || i >= length || data[i] != '=') {
      ++i;  // not a key here; step past and keep looking
      continue;
    }
    const std::string key(reinterpret_cast<const char*>(data + keyStart), i - keyStart);
    ++i;  // consume '='

    const size_t valueStart = i;
    while (i < length && data[i] != ',' && data[i] != '\t') ++i;
    if (i == valueStart) continue;  // empty value: the pattern requires at least one char
    pairs.emplace_back(key, std::string(reinterpret_cast<const char*>(data + valueStart),
                                         i - valueStart));
  }
  return pairs;
}

// --- data tables --------------------------------------------------------------------------

Bytes EncodeDataTableConfig(bool includeSessionBlock, uint16_t sessionId, int32_t requestNr) {
  return EncodeFrame(kCommandDataTableCfg, sessionId, requestNr, {}, includeSessionBlock);
}

DataTableConfigResponse DecodeDataTableConfig(const Bytes& data, bool includeSessionBlock) {
  DataTableConfigResponse response;
  const DecodedFrame frame = DecodeFrameGeneric(data, includeSessionBlock);
  response.status = frame.status;
  if (frame.status != ResponseStatus::kOk) return response;

  // One table per '\n'-separated line of "name=index,field=Tindex,..." text.
  size_t lineStart = 0;
  while (lineStart <= frame.payload.size()) {
    size_t lineEnd = lineStart;
    while (lineEnd < frame.payload.size() && frame.payload[lineEnd] != '\n') ++lineEnd;
    if (lineEnd > lineStart) {
      const auto pairs = ParseKeyValuePairs(frame.payload.data() + lineStart, lineEnd - lineStart);
      if (!pairs.empty()) {
        DataTableConfig table;
        table.name = pairs.front().first;
        table.index = std::atoi(pairs.front().second.c_str());
        for (size_t p = 1; p < pairs.size(); ++p) {
          const std::string& descriptor = pairs[p].second;
          if (descriptor.empty()) continue;
          DataTableField field;
          field.name = pairs[p].first;
          field.type = descriptor[0];
          field.index = std::atoi(descriptor.c_str() + 1);
          table.fields.push_back(std::move(field));
        }
        response.tables.push_back(std::move(table));
      }
    }
    if (lineEnd >= frame.payload.size()) break;
    lineStart = lineEnd + 1;
  }
  return response;
}

Bytes EncodeGetData(int tableIndex, const std::vector<int>& fieldIndexes, bool includeSessionBlock,
                    uint16_t sessionId, int32_t requestNr) {
  Bytes payload;
  AppendU8(payload, static_cast<uint8_t>(tableIndex));
  AppendU8(payload, static_cast<uint8_t>(fieldIndexes.size()));
  for (int index : fieldIndexes) AppendU8(payload, static_cast<uint8_t>(index));
  AppendU8(payload, 0x00);  // two reserved bytes, purpose undocumented upstream
  AppendU8(payload, 0x00);
  return EncodeFrame(kCommandGetData, sessionId, requestNr, payload, includeSessionBlock);
}

GetDataResponse DecodeGetData(const Bytes& data, int expectedTableIndex,
                              const std::vector<DataTableField>& tableFields,
                              bool includeSessionBlock) {
  GetDataResponse response;
  const DecodedFrame frame = DecodeFrameGeneric(data, includeSessionBlock);
  switch (frame.status) {
    case ResponseStatus::kIncomplete: return response;
    case ResponseStatus::kMalformed:
      response.status = GetDataStatus::kMalformed;
      return response;
    case ResponseStatus::kRejected:
      response.status = GetDataStatus::kRejected;
      return response;
    case ResponseStatus::kOk: break;
  }

  const Bytes& payload = frame.payload;
  if (payload.size() < 2) {
    response.status = GetDataStatus::kMalformed;
    return response;
  }
  if (payload[0] != static_cast<uint8_t>(expectedTableIndex)) {
    response.status = GetDataStatus::kTableMismatch;
    return response;
  }
  const size_t fieldCount = payload[1];
  if (payload.size() < 2 + fieldCount) {
    response.status = GetDataStatus::kMalformed;
    return response;
  }

  // The reply echoes which field indexes it is returning, in its own order,
  // which need not match the table config's order.
  std::vector<const DataTableField*> returnedFields;
  returnedFields.reserve(fieldCount);
  for (size_t i = 0; i < fieldCount; ++i) {
    const int returnedIndex = payload[2 + i];
    const DataTableField* match = nullptr;
    for (const DataTableField& field : tableFields) {
      if (field.index == returnedIndex) {
        match = &field;
        break;
      }
    }
    if (match == nullptr) {
      response.status = GetDataStatus::kMalformed;
      return response;
    }
    returnedFields.push_back(match);
  }

  size_t offset = 2 + fieldCount;
  if (returnedFields.empty() && offset != payload.size()) {
    response.status = GetDataStatus::kMalformed;
    return response;
  }

  while (offset < payload.size()) {
    nlohmann::json record = nlohmann::json::object();
    for (const DataTableField* field : returnedFields) {
      if (offset >= payload.size()) {
        response.status = GetDataStatus::kMalformed;
        response.records.clear();
        return response;
      }
      const size_t valueSize = payload[offset++];
      if (valueSize > payload.size() - offset) {
        response.status = GetDataStatus::kMalformed;
        response.records.clear();
        return response;
      }

      if (field->type == 'i' || field->type == 'L') {
        if (valueSize > sizeof(int64_t)) {
          response.status = GetDataStatus::kMalformed;
          response.records.clear();
          return response;
        }
        int64_t value = 0;
        for (size_t b = 0; b < valueSize; ++b) {
          value |= static_cast<int64_t>(payload[offset + b]) << (8 * b);
        }
        record[field->name] = value;
      } else if (field->type == 's') {
        record[field->name] =
            std::string(reinterpret_cast<const char*>(payload.data() + offset), valueSize);
      } else {
        response.status = GetDataStatus::kMalformed;
        response.records.clear();
        return response;
      }
      offset += valueSize;
    }
    response.records.push_back(std::move(record));
  }

  response.status = GetDataStatus::kOk;
  return response;
}

}  // namespace hsf::c3
