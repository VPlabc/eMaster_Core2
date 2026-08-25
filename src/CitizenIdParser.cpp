#include "hsf/CitizenIdParser.h"

#include <regex>
#include <vector>

namespace hsf {

namespace {
// The "code" field is documented as 7 digits but the sample string in the
// spec actually contains 8, so this matches a variable-length digit run
// rather than a fixed width to tolerate that discrepancy.
const std::regex kPattern(
    R"(^IDVNM(\d{10})(\d{12})<<(.{8})([A-Z])(\d+)VNM<{11}(\d)([A-Z<]+)$)");

std::vector<std::string> SplitNameTokens(const std::string& nameSection) {
  std::vector<std::string> tokens;
  std::string current;
  for (char c : nameSection) {
    if (c == '<') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) tokens.push_back(current);
  return tokens;
}
}  // namespace

std::optional<CitizenIdData> CitizenIdParser::Parse(const std::string& rawText) {
  std::smatch match;
  if (!std::regex_match(rawText, match, kPattern)) {
    return std::nullopt;
  }

  CitizenIdData data;
  data.raw_text = rawText;
  data.serial_number = match[1].str();
  data.citizen_id = match[2].str();

  std::string birthBlock = match[3].str();
  data.birth_date = birthBlock.substr(1, 6);

  data.gender = match[4].str();
  data.country = "VNM";

  std::vector<std::string> nameTokens = SplitNameTokens(match[7].str());
  if (!nameTokens.empty()) data.last_name = nameTokens[0];
  if (nameTokens.size() == 2) {
    data.first_name = nameTokens[1];
  } else if (nameTokens.size() >= 3) {
    data.middle_name = nameTokens[1];
    data.first_name = nameTokens[2];
  }

  return data;
}

nlohmann::json CitizenIdParser::ToJson(const CitizenIdData& data) {
  return nlohmann::json{
      {"serial_number", data.serial_number},
      {"citizen_id", data.citizen_id},
      {"birth_date", data.birth_date},
      {"gender", data.gender},
      {"country", data.country},
      {"last_name", data.last_name},
      {"middle_name", data.middle_name},
      {"first_name", data.first_name},
      {"raw_text", data.raw_text},
  };
}

}  // namespace hsf
