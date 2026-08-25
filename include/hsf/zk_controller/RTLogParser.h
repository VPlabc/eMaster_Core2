#pragma once

#include <string>
#include <vector>

#include "hsf/zk_controller/RTLogEvent.h"

namespace hsf {

struct RTLogParseResult {
  std::vector<RTLogEvent> events;
  std::vector<DoorAlarmStatus> statuses;
};

// Ported from ZKTecoProtocol/src/rtlog/rtlog_parser.cpp (same algorithm,
// std::string instead of QByteArray/QString). buffer is exactly what
// PullSdkClient::GetRTLog() returned -- ASCII CSV records separated by
// "\r\n" (sdk-protocol-reference.md's GetRTLog buffer format, Attachment
// 7), NUL-padded to the requested buffer size.
RTLogParseResult ParseRTLogBuffer(const std::string& buffer);

}  // namespace hsf
