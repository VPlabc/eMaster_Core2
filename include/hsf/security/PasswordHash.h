#pragma once

#include <cstddef>
#include <string>

namespace hsf {

// Password hashing and the secure-random source, over libsodium
// (request/AdvanceUpdate.md section 1.1).
//
// Argon2id via crypto_pwhash_str, which produces a self-describing PHC string:
//
//   $argon2id$v=19$m=65536,t=2,p=1$<salt>$<hash>
//
// The salt and the cost parameters travel inside the stored string, so raising
// the cost later does not invalidate existing hashes -- Verify reads whatever
// parameters each row was written with. That is the whole reason for using the
// library's string API rather than hashing into a fixed-size buffer ourselves.
//
// UNLIKE THE REST OF THIS CODEBASE, THERE IS NO STUB. MqClient, the ZK PullSDK
// and update signature verification all degrade to a stub when their library
// is absent, because each is an optional feature. Authentication is not: with
// auth.enabled defaulting on, a build that cannot hash a password is a build
// whose web UI nobody can log into. libsodium is therefore a hard dependency
// in CMakeLists.txt, and a missing one is a configure error rather than a
// surprise at first login.
class PasswordHash {
 public:
  // PHC string on success, empty on failure (which only happens if libsodium
  // cannot allocate the ~64 MB Argon2id working set -- worth handling, since
  // this runs on boards with 512 MB).
  static std::string Hash(const std::string& password);

  // Constant-time inside libsodium. False for a malformed or empty stored
  // hash, so a row that was never populated cannot be matched by anything.
  static bool Verify(const std::string& storedHash, const std::string& password);

  // True when `storedHash` was written with weaker parameters than the ones
  // Hash() uses now, i.e. the password should be re-hashed on next successful
  // login. Cheap to call; it only parses the PHC header.
  static bool NeedsRehash(const std::string& storedHash);

  // --- secure random -------------------------------------------------------
  //
  // Session tokens come from here, not from std::mt19937 or rand(). A
  // predictable token is the same as no token at all
  // (request/AdvanceUpdate.md section 1.1, "Do not use predictable tokens").

  // `bytes` of randomness, hex encoded (so the string is 2 * bytes long).
  static std::string RandomHex(size_t bytes);

  // A URL-safe random string suitable for a bearer token or an initial
  // password. Alphabet excludes look-alike characters, because initial
  // passwords get read off a screen and typed.
  static std::string RandomToken(size_t characters);

  // Constant-time comparison for secrets that are compared outside libsodium
  // (a token looked up by value, say). Length is not secret; content is.
  static bool ConstantTimeEquals(const std::string& a, const std::string& b);

  // Must be called once before anything else here. Returns false if libsodium
  // failed to initialise, which the caller should treat as fatal.
  static bool Initialise();
};

}  // namespace hsf
