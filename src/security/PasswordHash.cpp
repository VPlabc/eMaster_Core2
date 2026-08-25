#include "hsf/security/PasswordHash.h"

#include <sodium.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "hsf/Logger.h"

namespace hsf {
namespace {

// crypto_pwhash's INTERACTIVE profile: ~64 MB and a couple of passes, aimed at
// roughly 0.1 s on a desktop core. MODERATE/SENSITIVE would be stronger, but
// they want 256 MB / 1 GB, and this gateway runs on boards where that is most
// or all of RAM -- a login that swaps the card-reading thread out is a worse
// outcome than a marginally cheaper hash. The parameters are recorded in every
// stored hash, so this can be raised later without invalidating anything.
constexpr unsigned long long kOpsLimit = crypto_pwhash_OPSLIMIT_INTERACTIVE;
constexpr size_t kMemLimit = crypto_pwhash_MEMLIMIT_INTERACTIVE;

std::once_flag g_initOnce;
bool g_initialised = false;

}  // namespace

bool PasswordHash::Initialise() {
  std::call_once(g_initOnce, [] {
    // sodium_init() returns 1 if another caller already did it -- both are
    // success; only a negative return is a failure.
    g_initialised = sodium_init() >= 0;
    if (!g_initialised) {
      Logger::Instance().Error(LogCategory::System,
                               "libsodium failed to initialise; authentication cannot work");
    }
  });
  return g_initialised;
}

std::string PasswordHash::Hash(const std::string& password) {
  if (!Initialise()) return std::string();

  char encoded[crypto_pwhash_STRBYTES];
  if (crypto_pwhash_str(encoded, password.c_str(), password.size(), kOpsLimit, kMemLimit) != 0) {
    // Documented as "out of memory". On a small board that is a real
    // possibility, and returning an empty hash (which Verify always rejects)
    // is safer than storing something weaker as a fallback.
    Logger::Instance().Error(LogCategory::System,
                             "Argon2id hashing failed -- out of memory. The account was not changed.");
    return std::string();
  }
  return std::string(encoded);
}

bool PasswordHash::Verify(const std::string& storedHash, const std::string& password) {
  if (!Initialise()) return false;
  // An empty or over-long stored hash cannot be a valid PHC string. Checking
  // here keeps a half-populated row from ever being matched, and keeps a
  // non-terminated buffer out of crypto_pwhash_str_verify.
  if (storedHash.empty() || storedHash.size() >= crypto_pwhash_STRBYTES) return false;

  return crypto_pwhash_str_verify(storedHash.c_str(), password.c_str(), password.size()) == 0;
}

bool PasswordHash::NeedsRehash(const std::string& storedHash) {
  if (!Initialise()) return false;
  if (storedHash.empty() || storedHash.size() >= crypto_pwhash_STRBYTES) return false;
  // 1 = needs rehash, 0 = current, -1 = unparseable. Only a definite 1 counts;
  // rehashing something we could not parse would silently replace a hash we do
  // not understand.
  return crypto_pwhash_str_needs_rehash(storedHash.c_str(), kOpsLimit, kMemLimit) == 1;
}

std::string PasswordHash::RandomHex(size_t bytes) {
  if (!Initialise() || bytes == 0) return std::string();

  std::vector<unsigned char> buffer(bytes);
  randombytes_buf(buffer.data(), buffer.size());

  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(bytes * 2);
  for (unsigned char byte : buffer) {
    out.push_back(kHex[byte >> 4]);
    out.push_back(kHex[byte & 0x0F]);
  }
  sodium_memzero(buffer.data(), buffer.size());
  return out;
}

std::string PasswordHash::RandomToken(size_t characters) {
  if (!Initialise() || characters == 0) return std::string();

  // No 0/O/1/l/I. An initial admin password is read off a console and typed by
  // hand, and "was that a one or an ell" is a support call.
  static const char kAlphabet[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
  constexpr uint32_t kAlphabetSize = sizeof(kAlphabet) - 1;

  std::string out;
  out.reserve(characters);
  for (size_t i = 0; i < characters; ++i) {
    // randombytes_uniform is the unbiased form. Taking a random byte modulo 57
    // would quietly favour the first few characters of the alphabet.
    out.push_back(kAlphabet[randombytes_uniform(kAlphabetSize)]);
  }
  return out;
}

bool PasswordHash::ConstantTimeEquals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  if (a.empty()) return true;
  return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
}

}  // namespace hsf
