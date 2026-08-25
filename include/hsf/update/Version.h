#pragma once

#include <string>

namespace hsf {

// Semantic version, as it appears in the repo-root VERSION file, in a Git tag
// and in an update manifest -- the three that CMakeLists.txt and release.yml
// already refuse to let drift apart.
//
// Deliberately tolerant on input and strict on comparison: a manifest is
// written by a release pipeline we control, but it arrives over the network,
// so Parse() must never throw or read past the string. Anything it cannot make
// sense of is "not a version", which the caller reads as "no update".
struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;
  // Everything after "-" (e.g. "rc.1"). A prerelease sorts BEFORE the same
  // major.minor.patch with no suffix, per semver: 1.2.3-rc.1 < 1.2.3.
  std::string prerelease;

  // Accepts "1.2.3", "v1.2.3", "1.2.3-rc.1" and "1.2.3+build" (build metadata
  // is parsed off and ignored, again per semver -- it never affects ordering).
  // Returns false and leaves `out` untouched on anything else.
  static bool Parse(const std::string& text, Version& out);

  std::string ToString() const;

  // -1 / 0 / +1, semver precedence.
  int Compare(const Version& other) const;

  bool operator<(const Version& o) const { return Compare(o) < 0; }
  bool operator>(const Version& o) const { return Compare(o) > 0; }
  bool operator==(const Version& o) const { return Compare(o) == 0; }
  bool operator<=(const Version& o) const { return Compare(o) <= 0; }
  bool operator>=(const Version& o) const { return Compare(o) >= 0; }
};

// True when `candidate` is a valid version strictly newer than `current`.
// False if either fails to parse -- an unparseable manifest version must not
// be treated as an upgrade.
bool IsNewer(const std::string& candidate, const std::string& current);

}  // namespace hsf
