/* HSF Plugin SDK — API version and ABI macros.
 *
 * The Plugin API is versioned INDEPENDENTLY of the gateway application
 * (plan §6). A gateway at 1.0.0 may host Plugin API 1.0; the two numbers are
 * unrelated and the host must never assume otherwise.
 *
 * Compatibility rule, deliberately asymmetric:
 *
 *   MAJOR must match exactly.  A major bump means a struct changed shape or a
 *                              vtable slot changed meaning. Loading across it
 *                              is undefined behaviour, not a degraded mode.
 *   MINOR host >= plugin.      Minor bumps only APPEND vtable slots and struct
 *                              fields. A newer host can run an older plugin;
 *                              an older host cannot run a newer plugin, because
 *                              the plugin may call a slot the host lacks.
 *
 * Every versioned struct also carries `struct_size` as its first field. That is
 * what makes appending fields safe: the reader checks the size it was given
 * before touching anything a later minor added. Both mechanisms are needed —
 * the version says "what contract", the size says "how much of it is here".
 */
#ifndef HSF_PLUGIN_SDK_VERSION_H
#define HSF_PLUGIN_SDK_VERSION_H

#include <stddef.h>
#include <stdint.h>

#define HSF_PLUGIN_API_VERSION_MAJOR 1
#define HSF_PLUGIN_API_VERSION_MINOR 0

/* Packed form, for the single integer a manifest check compares. */
#define HSF_PLUGIN_API_VERSION \
  ((uint32_t)((HSF_PLUGIN_API_VERSION_MAJOR << 16) | HSF_PLUGIN_API_VERSION_MINOR))

#define HSF_API_VERSION_MAJOR_OF(v) ((uint32_t)(v) >> 16)
#define HSF_API_VERSION_MINOR_OF(v) ((uint32_t)(v) & 0xFFFFu)

/* Non-zero when a host at `host_v` may load a plugin built against
 * `plugin_v`. Written as a macro rather than a function so both sides can
 * evaluate it without linking anything. */
#define HSF_API_VERSION_COMPATIBLE(host_v, plugin_v)             \
  (HSF_API_VERSION_MAJOR_OF(host_v) == HSF_API_VERSION_MAJOR_OF(plugin_v) && \
   HSF_API_VERSION_MINOR_OF(host_v) >= HSF_API_VERSION_MINOR_OF(plugin_v))

/* Symbol visibility. A plugin exports exactly three symbols (see plugin.h);
 * everything else should stay hidden so two plugins that happen to share an
 * internal symbol name cannot collide at load time. Build with
 * -fvisibility=hidden and mark the entry points HSF_PLUGIN_EXPORT. */
#if defined(_WIN32)
#  define HSF_PLUGIN_EXPORT __declspec(dllexport)
#  define HSF_PLUGIN_IMPORT __declspec(dllimport)
#else
#  define HSF_PLUGIN_EXPORT __attribute__((visibility("default")))
#  define HSF_PLUGIN_IMPORT
#endif

/* The ABI is C. C++ plugins are welcome — internally they may use anything —
 * but nothing with a C++ type may cross the boundary (plan §4). */
#if defined(__cplusplus)
#  define HSF_ABI_BEGIN extern "C" {
#  define HSF_ABI_END   }
#else
#  define HSF_ABI_BEGIN
#  define HSF_ABI_END
#endif

/* Platform triple, as it appears in a manifest's "platforms" array and in a
 * package filename. Checked before load so a wrong-architecture .so is
 * rejected with a clear message instead of a loader error (plan §13). */
#if defined(_WIN32)
#  if defined(_M_X64) || defined(__x86_64__)
#    define HSF_PLATFORM_TRIPLE "windows-x86_64"
#  else
#    define HSF_PLATFORM_TRIPLE "windows-x86"
#  endif
#elif defined(__APPLE__)
#  if defined(__aarch64__)
#    define HSF_PLATFORM_TRIPLE "macos-arm64"
#  else
#    define HSF_PLATFORM_TRIPLE "macos-x86_64"
#  endif
#elif defined(__linux__)
#  if defined(__aarch64__)
#    define HSF_PLATFORM_TRIPLE "linux-arm64"
#  elif defined(__arm__)
#    define HSF_PLATFORM_TRIPLE "linux-armhf"
#  elif defined(__x86_64__)
#    define HSF_PLATFORM_TRIPLE "linux-x86_64"
#  else
#    define HSF_PLATFORM_TRIPLE "linux-unknown"
#  endif
#else
#  define HSF_PLATFORM_TRIPLE "unknown"
#endif

#endif /* HSF_PLUGIN_SDK_VERSION_H */
