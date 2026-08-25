/* HSF Plugin SDK — a test harness in one header, with no dependencies.
 *
 * WHY NOT CATCH2 OR GTEST. Plan §26: "Do not require plugin developers to
 * install the entire Gateway development environment. The SDK must provide the
 * minimum required development dependencies." A plugin author cross-compiling
 * for linux-armhf should not also have to get a test framework through vcpkg
 * for the host architecture. This is a few hundred lines, has no build step,
 * and works anywhere a C++17 compiler does.
 *
 * The gateway itself has no C++ test framework today either
 * (docs/architecture/current-state.md — tests are Lua scripts and ad-hoc
 * programs), so this is additive, not a competing choice.
 *
 *   HSF_TEST(name) { ... }        registers and runs
 *   HSF_CHECK(cond)               records a failure, keeps going
 *   HSF_REQUIRE(cond)             records a failure, abandons this test
 *   HSF_CHECK_EQ(a, b)            prints both sides when it fails
 *   HSF_CHECK_STATUS(expr, want)  prints status NAMES, not raw integers
 *
 * int main() { return hsf::test::RunAll(); }  — or link hsf_test_main.
 */
#ifndef HSF_PLUGIN_SDK_TESTING_HPP
#define HSF_PLUGIN_SDK_TESTING_HPP

#include "hsf/error.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace hsf {
namespace test {

struct Failure {
  std::string file;
  int         line;
  std::string text;
};

class Context {
 public:
  void Fail(const char* file, int line, const std::string& text) {
    failures_.push_back({file, line, text});
  }
  bool Failed() const { return !failures_.empty(); }
  const std::vector<Failure>& Failures() const { return failures_; }

 private:
  std::vector<Failure> failures_;
};

/* Thrown by HSF_REQUIRE to abandon one test without killing the run. Caught in
 * RunAll; never escapes. */
struct Abandon {};

struct Case {
  std::string name;
  std::function<void(Context&)> fn;
};

inline std::vector<Case>& Registry() {
  static std::vector<Case> cases;
  return cases;
}

struct Registrar {
  Registrar(const char* name, std::function<void(Context&)> fn) {
    Registry().push_back({name, std::move(fn)});
  }
};

/* Renders anything streamable, so a failure shows values rather than just the
 * expression that produced them. */
template <typename T>
inline std::string Show(const T& v) {
  std::ostringstream os;
  os << v;
  return os.str();
}
inline std::string Show(bool v) { return v ? "true" : "false"; }
inline std::string Show(const std::string& v) { return "\"" + v + "\""; }
inline std::string Show(const char* v) {
  return v ? "\"" + std::string(v) + "\"" : "(null)";
}
/* Unsigned char and friends would otherwise print as unreadable glyphs. */
inline std::string Show(uint8_t v) { return std::to_string(static_cast<unsigned>(v)); }

inline std::string ShowBytes(const std::vector<uint8_t>& b) {
  std::ostringstream os;
  os << b.size() << " bytes [";
  for (size_t i = 0; i < b.size(); ++i) {
    if (i) os << ' ';
    if (i == 16 && b.size() > 20) { os << "..."; break; }
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%02X", b[i]);
    os << buf;
  }
  os << ']';
  return os.str();
}

/* `only` runs just the cases whose name contains it; argv[1] supplies it. */
inline int RunAll(const char* only = nullptr) {
  size_t passed = 0, failed = 0, skipped = 0;
  for (const Case& c : Registry()) {
    if (only && *only && c.name.find(only) == std::string::npos) {
      ++skipped;
      continue;
    }
    Context ctx;
    try {
      c.fn(ctx);
    } catch (const Abandon&) {
      /* already recorded */
    } catch (const std::exception& e) {
      ctx.Fail(__FILE__, 0, std::string("unexpected exception: ") + e.what());
    } catch (...) {
      ctx.Fail(__FILE__, 0, "unexpected non-standard exception");
    }

    if (ctx.Failed()) {
      ++failed;
      std::printf("FAIL  %s\n", c.name.c_str());
      for (const Failure& f : ctx.Failures()) {
        std::printf("        %s:%d  %s\n", f.file.c_str(), f.line, f.text.c_str());
      }
    } else {
      ++passed;
      std::printf("ok    %s\n", c.name.c_str());
    }
  }
  std::printf("\n%zu passed, %zu failed", passed, failed);
  if (skipped) std::printf(", %zu skipped", skipped);
  std::printf("\n");
  return failed == 0 ? 0 : 1;
}

}  // namespace test
}  // namespace hsf

/* Two levels of macro so __LINE__ expands before pasting. */
#define HSF_TEST_CAT_(a, b) a##b
#define HSF_TEST_CAT(a, b) HSF_TEST_CAT_(a, b)

#define HSF_TEST(name_literal)                                                 \
  static void HSF_TEST_CAT(hsf_test_fn_, __LINE__)(::hsf::test::Context&);      \
  static ::hsf::test::Registrar HSF_TEST_CAT(hsf_test_reg_, __LINE__)(          \
      name_literal, HSF_TEST_CAT(hsf_test_fn_, __LINE__));                      \
  static void HSF_TEST_CAT(hsf_test_fn_, __LINE__)(::hsf::test::Context& hsf_ctx)

#define HSF_CHECK(cond)                                                        \
  do {                                                                         \
    if (!(cond)) hsf_ctx.Fail(__FILE__, __LINE__, "expected: " #cond);          \
  } while (0)

#define HSF_REQUIRE(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      hsf_ctx.Fail(__FILE__, __LINE__, "required: " #cond);                     \
      throw ::hsf::test::Abandon{};                                            \
    }                                                                          \
  } while (0)

#define HSF_CHECK_EQ(a, b)                                                     \
  do {                                                                         \
    auto hsf_a_ = (a);                                                         \
    auto hsf_b_ = (b);                                                         \
    if (!(hsf_a_ == hsf_b_)) {                                                 \
      hsf_ctx.Fail(__FILE__, __LINE__,                                         \
                   std::string(#a " == " #b "  (") +                            \
                       ::hsf::test::Show(hsf_a_) + " vs " +                     \
                       ::hsf::test::Show(hsf_b_) + ")");                        \
    }                                                                          \
  } while (0)

#define HSF_CHECK_NE(a, b)                                                     \
  do {                                                                         \
    auto hsf_a_ = (a);                                                         \
    auto hsf_b_ = (b);                                                         \
    if (hsf_a_ == hsf_b_) {                                                    \
      hsf_ctx.Fail(__FILE__, __LINE__,                                         \
                   std::string(#a " != " #b "  (both ") +                       \
                       ::hsf::test::Show(hsf_a_) + ")");                        \
    }                                                                          \
  } while (0)

/* Statuses print by name. "expected OK, got PROTOCOL" beats "expected 0,
 * got -9" every time you read a CI log. */
#define HSF_CHECK_STATUS(expr, want)                                           \
  do {                                                                         \
    HSFStatus hsf_got_ = (expr);                                               \
    HSFStatus hsf_want_ = (want);                                              \
    if (hsf_got_ != hsf_want_) {                                               \
      hsf_ctx.Fail(__FILE__, __LINE__,                                         \
                   std::string(#expr ": expected ") +                           \
                       hsf_status_name(hsf_want_) + ", got " +                   \
                       hsf_status_name(hsf_got_));                              \
    }                                                                          \
  } while (0)

#define HSF_CHECK_OK(expr) HSF_CHECK_STATUS(expr, HSF_OK)

#define HSF_TEST_MAIN()                                                        \
  int main(int argc, char** argv) {                                            \
    return ::hsf::test::RunAll(argc > 1 ? argv[1] : nullptr);                   \
  }

#endif /* HSF_PLUGIN_SDK_TESTING_HPP */
