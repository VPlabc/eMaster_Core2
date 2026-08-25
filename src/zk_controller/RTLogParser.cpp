#include "hsf/zk_controller/RTLogParser.h"

#include <cstdlib>

namespace hsf {
namespace {

std::string Trim(const std::string& s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::vector<std::string> Split(const std::string& s, char delim) {
  std::vector<std::string> result;
  size_t start = 0;
  while (start <= s.size()) {
    size_t pos = s.find(delim, start);
    if (pos == std::string::npos) {
      result.push_back(s.substr(start));
      break;
    }
    result.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
  return result;
}

// Empty lines dropped -- matches the original's Qt::SkipEmptyParts.
std::vector<std::string> SplitLines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start <= text.size()) {
    size_t pos = text.find("\r\n", start);
    std::string line = pos == std::string::npos ? text.substr(start) : text.substr(start, pos - start);
    if (!line.empty()) lines.push_back(line);
    if (pos == std::string::npos) break;
    start = pos + 2;
  }
  return lines;
}

}  // namespace

RTLogParseResult ParseRTLogBuffer(const std::string& buffer) {
  RTLogParseResult result;

  // GetRTLog()'s buffer is fixed-size and NUL-padded after the real
  // content; constructing via c_str() truncates at the first NUL, which is
  // safe here since real RTLog CSV content is pure ASCII with no embedded
  // NUL bytes.
  std::string text = buffer.c_str();

  for (const std::string& line : SplitLines(text)) {
    std::vector<std::string> fields = Split(line, ',');
    if (fields.size() < 7) continue;  // malformed/short line (e.g. trailing padding) -- skip

    if (Trim(fields[4]) == "255") {
      DoorAlarmStatus status;
      status.time = fields[0];
      status.dssStatusPacked = static_cast<uint32_t>(std::strtoul(Trim(fields[1]).c_str(), nullptr, 10));
      status.alarmStatusPacked = static_cast<uint32_t>(std::strtoul(Trim(fields[2]).c_str(), nullptr, 10));
      result.statuses.push_back(status);
    } else {
      RTLogEvent event;
      event.time = fields[0];
      event.pin = fields[1];
      event.cardNo = fields[2];
      event.doorNo = std::atoi(Trim(fields[3]).c_str());
      event.eventType = std::atoi(Trim(fields[4]).c_str());
      event.inOutStatus = std::atoi(Trim(fields[5]).c_str());
      event.verifyMode = std::atoi(Trim(fields[6]).c_str());
      result.events.push_back(event);
    }
  }

  return result;
}

}  // namespace hsf
