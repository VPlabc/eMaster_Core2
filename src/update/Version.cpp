#include "hsf/update/Version.h"

#include <cctype>
#include <cstdlib>
#include <vector>

namespace hsf {
namespace {

// Reads decimal digits from `text` starting at `pos`, advancing it. Returns
// false on no digits at all, or on a run long enough to overflow -- a manifest
// claiming version 99999999999999999999.0.0 is malformed, not enormous.
bool ReadNumber(const std::string& text, size_t& pos, int& out) {
  const size_t start = pos;
  long long value = 0;
  while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
    value = value * 10 + (text[pos] - '0');
    if (value > 1000000000LL) return false;
    ++pos;
  }
  if (pos == start) return false;
  out = static_cast<int>(value);
  return true;
}

// Split a prerelease string on '.' into its dot-separated identifiers, which
// is the unit semver compares.
std::vector<std::string> SplitIdentifiers(const std::string& text) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (start <= text.size()) {
    size_t dot = text.find('.', start);
    if (dot == std::string::npos) {
      parts.push_back(text.substr(start));
      break;
    }
    parts.push_back(text.substr(start, dot - start));
    start = dot + 1;
  }
  return parts;
}

bool IsNumericIdentifier(const std::string& text) {
  if (text.empty()) return false;
  for (char c : text) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

}  // namespace

bool Version::Parse(const std::string& text, Version& out) {
  size_t pos = 0;
  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
  // Tags are "v1.2.3"; the VERSION file and manifests are "1.2.3". Accept both
  // so a caller never has to remember which side it is holding.
  if (pos < text.size() && (text[pos] == 'v' || text[pos] == 'V')) ++pos;

  Version parsed;
  if (!ReadNumber(text, pos, parsed.major)) return false;
  if (pos >= text.size() || text[pos] != '.') return false;
  ++pos;
  if (!ReadNumber(text, pos, parsed.minor)) return false;
  if (pos >= text.size() || text[pos] != '.') return false;
  ++pos;
  if (!ReadNumber(text, pos, parsed.patch)) return false;

  if (pos < text.size() && text[pos] == '-') {
    ++pos;
    const size_t plus = text.find('+', pos);
    parsed.prerelease = text.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos);
    pos = (plus == std::string::npos) ? text.size() : plus;
  }
  // Build metadata: allowed, ignored.
  if (pos < text.size() && text[pos] == '+') pos = text.size();

  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
  if (pos != text.size()) return false;

  out = parsed;
  return true;
}

std::string Version::ToString() const {
  std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
  if (!prerelease.empty()) s += "-" + prerelease;
  return s;
}

int Version::Compare(const Version& other) const {
  if (major != other.major) return major < other.major ? -1 : 1;
  if (minor != other.minor) return minor < other.minor ? -1 : 1;
  if (patch != other.patch) return patch < other.patch ? -1 : 1;

  // 1.2.3-rc.1 precedes 1.2.3. Getting this backwards would ship a release
  // candidate as an "upgrade" over the final build it was cut for.
  if (prerelease.empty() && other.prerelease.empty()) return 0;
  if (prerelease.empty()) return 1;
  if (other.prerelease.empty()) return -1;

  const std::vector<std::string> a = SplitIdentifiers(prerelease);
  const std::vector<std::string> b = SplitIdentifiers(other.prerelease);
  const size_t count = a.size() < b.size() ? a.size() : b.size();
  for (size_t i = 0; i < count; ++i) {
    const bool na = IsNumericIdentifier(a[i]);
    const bool nb = IsNumericIdentifier(b[i]);
    if (na && nb) {
      const long long va = std::strtoll(a[i].c_str(), nullptr, 10);
      const long long vb = std::strtoll(b[i].c_str(), nullptr, 10);
      if (va != vb) return va < vb ? -1 : 1;
    } else if (na != nb) {
      return na ? -1 : 1;  // numeric identifiers have lower precedence
    } else if (a[i] != b[i]) {
      return a[i] < b[i] ? -1 : 1;
    }
  }
  if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
  return 0;
}

bool IsNewer(const std::string& candidate, const std::string& current) {
  Version c;
  Version now;
  if (!Version::Parse(candidate, c)) return false;
  if (!Version::Parse(current, now)) return false;
  return c > now;
}

}  // namespace hsf
