#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hsf {

// Backend input validation (request/AdvanceUpdate.md section 1.5) and the
// path-safety checks behind section 1.7.
//
// The frontend validates for the user's benefit; this validates because the
// frontend is not a security boundary and never was. Everything here is
// reject-by-default: a value that cannot be proven to fit is refused, and the
// caller gets a message naming the field but never echoing a secret.
class Validate {
 public:
  // Accumulates failures so one response can report every bad field, rather
  // than making the caller fix them one round trip at a time.
  class Errors {
   public:
    void Add(const std::string& field, const std::string& problem);
    bool Empty() const { return problems_.empty(); }
    // {"error":"VALIDATION_FAILED","fields":{"timeout":"must be 1..600"}}
    nlohmann::json ToJson() const;
    std::string Summary() const;

   private:
    std::vector<std::pair<std::string, std::string>> problems_;
  };

  // --- scalars -------------------------------------------------------------

  static bool StringField(const nlohmann::json& parent, const std::string& field, size_t minLength,
                          size_t maxLength, std::string& out, Errors& errors, bool required = true);

  // As above, plus an allowed-character predicate by name: "alnum",
  // "identifier" (alnum . _ -), "printable" (no control characters).
  static bool StringField(const nlohmann::json& parent, const std::string& field, size_t minLength,
                          size_t maxLength, const std::string& charset, std::string& out, Errors& errors,
                          bool required = true);

  static bool IntField(const nlohmann::json& parent, const std::string& field, int64_t minimum,
                       int64_t maximum, int64_t& out, Errors& errors, bool required = true);

  static bool BoolField(const nlohmann::json& parent, const std::string& field, bool& out, Errors& errors,
                        bool required = true);

  static bool EnumField(const nlohmann::json& parent, const std::string& field,
                        std::initializer_list<const char*> allowed, std::string& out, Errors& errors,
                        bool required = true);

  // --- whole-body checks ----------------------------------------------------

  // Parses `body` as a JSON object, enforcing a maximum size first so a
  // 500 MB body is rejected before the parser allocates anything.
  static bool JsonObject(const std::string& body, size_t maxBytes, nlohmann::json& out, std::string& error);

  // --- text hygiene -----------------------------------------------------------

  // Control characters (other than tab/newline) removed and length capped.
  // Used on anything that reaches the audit log or the runtime log, because a
  // value containing CR/LF can forge a second log line (section 1.6, "log
  // injection").
  static std::string SanitiseForLog(const std::string& value, size_t maxLength = 256);

  static bool IsValidUtf8(const std::string& value);

  // --- path safety --------------------------------------------------------------

  // True when `name` is a single, ordinary filename: no separators, no "..",
  // no leading dot, no control characters, no Windows reserved device name.
  static bool SafeFilename(const std::string& name);

  // True when joining `relative` onto `base` stays inside `base`, resolving
  // ".." and symlinks. The check every "give me a file" endpoint needs, and
  // the reason the spec calls out ../../../../etc/passwd by name.
  static bool PathStaysWithin(const std::string& base, const std::string& relative, std::string& resolved);
};

}  // namespace hsf
