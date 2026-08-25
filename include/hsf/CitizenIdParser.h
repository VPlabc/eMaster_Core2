#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace hsf {

struct CitizenIdData {
  std::string serial_number;
  std::string citizen_id;
  std::string birth_date;  // YYMMDD
  std::string gender;
  std::string country;
  std::string last_name;
  std::string middle_name;
  std::string first_name;
  std::string raw_text;
};

// Parses the fixed-format text block emitted by the USB Serial Citizen ID
// card reader, e.g.:
//   IDVNM0123456789012345678901<<59505041M01234567VNM<<<<<<<<<<<8NGUYEN<<VAN<SANG<<<<<<<<<<<<<<
class CitizenIdParser {
 public:
  static std::optional<CitizenIdData> Parse(const std::string& rawText);
  static nlohmann::json ToJson(const CitizenIdData& data);
};

}  // namespace hsf
