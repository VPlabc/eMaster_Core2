#include "hsf/security/Validation.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <system_error>

using nlohmann::json;
namespace fs = std::filesystem;

namespace hsf {
namespace {

bool CharsetAllows(const std::string& charset, unsigned char c) {
  if (charset == "alnum") {
    return std::isalnum(c) != 0;
  }
  if (charset == "identifier") {
    return std::isalnum(c) != 0 || c == '.' || c == '_' || c == '-';
  }
  if (charset == "printable") {
    // Tab is fine inside a text field; the rest of C0 and DEL are not.
    return c == '\t' || (c >= 0x20 && c != 0x7F);
  }
  return true;
}

// Windows refuses to create these as filenames whatever the extension, and a
// path containing one is a good sign somebody is probing rather than typing.
bool IsReservedDeviceName(const std::string& name) {
  static const char* kReserved[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
                                    "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                    "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  std::string stem = name.substr(0, name.find('.'));
  std::transform(stem.begin(), stem.end(), stem.begin(),
                 [](unsigned char c) { return static_cast<char>(::toupper(c)); });
  for (const char* reserved : kReserved) {
    if (stem == reserved) return true;
  }
  return false;
}

}  // namespace

void Validate::Errors::Add(const std::string& field, const std::string& problem) {
  problems_.emplace_back(field, problem);
}

json Validate::Errors::ToJson() const {
  json fields = json::object();
  for (const auto& [field, problem] : problems_) fields[field] = problem;
  return json{{"success", false}, {"error", "VALIDATION_FAILED"}, {"fields", fields}};
}

std::string Validate::Errors::Summary() const {
  std::string summary;
  for (const auto& [field, problem] : problems_) {
    if (!summary.empty()) summary += "; ";
    summary += field + ": " + problem;
  }
  return summary;
}

bool Validate::StringField(const json& parent, const std::string& field, size_t minLength, size_t maxLength,
                           std::string& out, Errors& errors, bool required) {
  return StringField(parent, field, minLength, maxLength, "printable", out, errors, required);
}

bool Validate::StringField(const json& parent, const std::string& field, size_t minLength, size_t maxLength,
                           const std::string& charset, std::string& out, Errors& errors, bool required) {
  if (!parent.is_object() || !parent.contains(field) || parent[field].is_null()) {
    if (required) errors.Add(field, "is required");
    return false;
  }
  if (!parent[field].is_string()) {
    errors.Add(field, "must be a string");
    return false;
  }
  const std::string value = parent[field].get<std::string>();
  if (value.size() < minLength || value.size() > maxLength) {
    errors.Add(field, "must be " + std::to_string(minLength) + "-" + std::to_string(maxLength) + " characters");
    return false;
  }
  if (!IsValidUtf8(value)) {
    errors.Add(field, "must be valid UTF-8");
    return false;
  }
  for (unsigned char c : value) {
    // Multi-byte UTF-8 continuation bytes are >= 0x80 and are accepted by the
    // UTF-8 check above; only ASCII is filtered by charset.
    if (c < 0x80 && !CharsetAllows(charset, c)) {
      errors.Add(field, "contains a disallowed character");
      return false;
    }
  }
  out = value;
  return true;
}

bool Validate::IntField(const json& parent, const std::string& field, int64_t minimum, int64_t maximum,
                        int64_t& out, Errors& errors, bool required) {
  if (!parent.is_object() || !parent.contains(field) || parent[field].is_null()) {
    if (required) errors.Add(field, "is required");
    return false;
  }
  if (!parent[field].is_number_integer() && !parent[field].is_number_unsigned()) {
    // A float where an int is expected is a client bug, and silently
    // truncating it makes that bug somebody's outage later.
    errors.Add(field, "must be an integer");
    return false;
  }
  const int64_t value = parent[field].get<int64_t>();
  if (value < minimum || value > maximum) {
    errors.Add(field, "must be " + std::to_string(minimum) + ".." + std::to_string(maximum));
    return false;
  }
  out = value;
  return true;
}

bool Validate::BoolField(const json& parent, const std::string& field, bool& out, Errors& errors,
                         bool required) {
  if (!parent.is_object() || !parent.contains(field) || parent[field].is_null()) {
    if (required) errors.Add(field, "is required");
    return false;
  }
  if (!parent[field].is_boolean()) {
    errors.Add(field, "must be true or false");
    return false;
  }
  out = parent[field].get<bool>();
  return true;
}

bool Validate::EnumField(const json& parent, const std::string& field,
                         std::initializer_list<const char*> allowed, std::string& out, Errors& errors,
                         bool required) {
  std::string value;
  if (!StringField(parent, field, 1, 64, "identifier", value, errors, required)) return false;
  for (const char* candidate : allowed) {
    if (value == candidate) {
      out = value;
      return true;
    }
  }
  std::string list;
  for (const char* candidate : allowed) {
    if (!list.empty()) list += ", ";
    list += candidate;
  }
  errors.Add(field, "must be one of: " + list);
  return false;
}

bool Validate::JsonObject(const std::string& body, size_t maxBytes, json& out, std::string& error) {
  // Size first. Parsing a body to discover it was too big is precisely the
  // work an attacker wanted to make the gateway do (section 1.12 C).
  if (body.size() > maxBytes) {
    error = "request body exceeds " + std::to_string(maxBytes) + " bytes";
    return false;
  }
  if (body.empty()) {
    out = json::object();
    return true;
  }
  // nlohmann throws on malformed input; an exception escaping into a Crow
  // worker would take the connection down and log a stack-shaped message.
  try {
    out = json::parse(body);
  } catch (const std::exception&) {
    error = "body is not valid JSON";
    return false;
  }
  if (!out.is_object()) {
    error = "body must be a JSON object";
    return false;
  }
  return true;
}

std::string Validate::SanitiseForLog(const std::string& value, size_t maxLength) {
  std::string clean;
  clean.reserve(std::min(value.size(), maxLength));
  for (unsigned char c : value) {
    if (clean.size() >= maxLength) {
      clean += "...";
      break;
    }
    // CR and LF are the whole point: without stripping them, a value can end a
    // log line and start a convincing fake one.
    if (c == '\r' || c == '\n' || c == '\t') {
      clean.push_back(' ');
    } else if (c < 0x20 || c == 0x7F) {
      clean.push_back('?');
    } else {
      clean.push_back(static_cast<char>(c));
    }
  }
  return clean;
}

bool Validate::IsValidUtf8(const std::string& value) {
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(value.data());
  const size_t length = value.size();
  size_t i = 0;
  while (i < length) {
    const unsigned char first = bytes[i];
    size_t extra = 0;
    unsigned int codepoint = 0;

    if (first < 0x80) {
      i++;
      continue;
    } else if ((first & 0xE0) == 0xC0) {
      extra = 1;
      codepoint = first & 0x1F;
    } else if ((first & 0xF0) == 0xE0) {
      extra = 2;
      codepoint = first & 0x0F;
    } else if ((first & 0xF8) == 0xF0) {
      extra = 3;
      codepoint = first & 0x07;
    } else {
      return false;  // continuation byte or 5/6-byte form
    }

    if (i + extra >= length) return false;
    for (size_t k = 1; k <= extra; ++k) {
      const unsigned char continuation = bytes[i + k];
      if ((continuation & 0xC0) != 0x80) return false;
      codepoint = (codepoint << 6) | (continuation & 0x3F);
    }

    // Overlong encodings and surrogates are the classic way to smuggle a '/'
    // or a NUL past a naive filter, so they are rejected rather than accepted
    // and normalised.
    if (extra == 1 && codepoint < 0x80) return false;
    if (extra == 2 && codepoint < 0x800) return false;
    if (extra == 3 && codepoint < 0x10000) return false;
    if (codepoint > 0x10FFFF) return false;
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return false;

    i += extra + 1;
  }
  return true;
}

bool Validate::SafeFilename(const std::string& name) {
  if (name.empty() || name.size() > 128) return false;
  if (name == "." || name == "..") return false;
  if (name.front() == '.') return false;   // no dotfiles, no "..foo"
  if (name.back() == '.' || name.back() == ' ') return false;  // Windows strips these silently
  for (unsigned char c : name) {
    if (c < 0x20 || c == 0x7F) return false;
    if (std::strchr("/\\:*?\"<>|", c) != nullptr) return false;
  }
  return !IsReservedDeviceName(name);
}

bool Validate::PathStaysWithin(const std::string& base, const std::string& relative, std::string& resolved) {
  std::error_code ec;
  const fs::path baseCanonical = fs::weakly_canonical(fs::path(base), ec);
  if (ec) return false;

  // An absolute `relative` would replace the base entirely under operator/,
  // which is how "/etc/passwd" becomes the resolved path on a naive join.
  const fs::path candidate(relative);
  if (candidate.is_absolute()) return false;
  for (const auto& part : candidate) {
    if (part == "..") return false;  // rejected before resolution, not after
  }

  const fs::path joined = fs::weakly_canonical(baseCanonical / candidate, ec);
  if (ec) return false;

  // Compare component-wise. A string prefix test would accept
  // "/opt/gateway-evil" for a base of "/opt/gateway".
  auto baseIt = baseCanonical.begin();
  auto joinedIt = joined.begin();
  for (; baseIt != baseCanonical.end(); ++baseIt, ++joinedIt) {
    if (joinedIt == joined.end() || *joinedIt != *baseIt) return false;
  }
  resolved = joined.string();
  return true;
}

}  // namespace hsf
