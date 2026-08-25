#include "hsf/lua_package/PackageKeys.h"

#include <sodium.h>

#include <filesystem>
#include <fstream>
#include <system_error>

#include "hsf/Logger.h"
#include "hsf/lua_package/LuaPackage.h"
#include "hsf/security/PasswordHash.h"
#include "hsf/update/Sha256.h"

namespace fs = std::filesystem;

namespace hsf {
namespace {

std::string Resolve(const std::string& dataDir, const std::string& name) {
  const fs::path candidate(name);
  if (candidate.is_absolute()) return candidate.string();
  return (fs::path(dataDir) / candidate).string();
}

// Keys are stored as hex on ONE line, not raw bytes. A key file that can be
// catted, diffed and pasted into a provisioning tool without a base64 step is
// worth the doubled size at 32 and 64 bytes, and it removes a whole class of
// "the editor added a newline / mangled a 0x1a" problem that binary key files
// invite on Windows.
std::string ToHex(const std::vector<unsigned char>& bytes) {
  static const char* kHex = "0123456789abcdef";
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (unsigned char byte : bytes) {
    hex.push_back(kHex[byte >> 4]);
    hex.push_back(kHex[byte & 0x0F]);
  }
  return hex;
}

bool FromHex(const std::string& text, std::vector<unsigned char>& out) {
  std::string hex;
  hex.reserve(text.size());
  for (char c : text) {
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    hex.push_back(c);
  }
  if (hex.empty() || hex.size() % 2 != 0) return false;

  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  out.clear();
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int high = nibble(hex[i]);
    const int low = nibble(hex[i + 1]);
    if (high < 0 || low < 0) return false;
    out.push_back(static_cast<unsigned char>((high << 4) | low));
  }
  return true;
}

}  // namespace

PackageKeys::PackageKeys(std::string dataDir) : dataDir_(std::move(dataDir)) {
  encryptionKeyPath_ = Resolve(dataDir_, "lua_package.key");
  publicKeyPath_ = Resolve(dataDir_, "lua_signing.pub");
  secretKeyPath_ = Resolve(dataDir_, "lua_signing.key");
}

bool PackageKeys::WriteKeyFile(const std::string& path, const std::vector<unsigned char>& key,
                               std::string& error) const {
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  {
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
      error = "cannot write " + path;
      return false;
    }
    out << ToHex(key) << "\n";
    if (!out.good()) {
      error = "failed while writing " + path;
      return false;
    }
  }
#if !defined(_WIN32)
  // Owner read/write only. On Windows this is a no-op and the directory ACL is
  // what protects the file -- called out in docs/lua-packaging.md rather than
  // silently assumed.
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
#endif
  return true;
}

bool PackageKeys::ReadKeyFile(const std::string& path, size_t expectedBytes,
                              std::vector<unsigned char>& out, std::string& error) const {
  std::ifstream in(path);
  if (!in.is_open()) {
    error = "key file not found: " + path;
    return false;
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!FromHex(text, out)) {
    error = "key file " + path + " is not valid hex";
    return false;
  }
  if (out.size() != expectedBytes) {
    error = "key file " + path + " holds " + std::to_string(out.size()) + " bytes, expected " +
            std::to_string(expectedBytes);
    out.clear();
    return false;
  }
  return true;
}

bool PackageKeys::EnsureExists(bool wantSigningKey, bool& generatedSigningKey, std::string& error) {
  generatedSigningKey = false;
  if (!PasswordHash::Initialise()) {
    error = "libsodium is not initialised";
    return false;
  }

  std::error_code ec;
  if (!fs::exists(encryptionKeyPath_, ec)) {
    std::vector<unsigned char> key(LuaPackage::kKeyBytes);
    randombytes_buf(key.data(), key.size());
    if (!WriteKeyFile(encryptionKeyPath_, key, error)) return false;
    sodium_memzero(key.data(), key.size());
    Logger::Instance().Info(LogCategory::Lua,
                            "Generated a Lua package encryption key at " + encryptionKeyPath_);
  }

  // The signing keypair is only created where packages are built. A gateway
  // that just runs them gets the public half provisioned and never holds the
  // secret, which is the whole reason the signature is worth more than the
  // encryption.
  if (wantSigningKey && !fs::exists(secretKeyPath_, ec)) {
    unsigned char publicKey[crypto_sign_PUBLICKEYBYTES];
    unsigned char secretKey[crypto_sign_SECRETKEYBYTES];
    if (crypto_sign_keypair(publicKey, secretKey) != 0) {
      error = "could not generate a signing keypair";
      return false;
    }
    std::vector<unsigned char> secret(secretKey, secretKey + sizeof(secretKey));
    std::vector<unsigned char> pub(publicKey, publicKey + sizeof(publicKey));
    if (!WriteKeyFile(secretKeyPath_, secret, error)) return false;
    if (!WriteKeyFile(publicKeyPath_, pub, error)) return false;
    sodium_memzero(secretKey, sizeof(secretKey));
    sodium_memzero(secret.data(), secret.size());
    generatedSigningKey = true;
  }

  // A device provisioned with only the secret key can still derive the public
  // one -- Ed25519 secret keys carry it in their second half -- so a missing
  // .pub is recoverable rather than fatal.
  if (!fs::exists(publicKeyPath_, ec) && fs::exists(secretKeyPath_, ec)) {
    std::vector<unsigned char> secret;
    if (ReadKeyFile(secretKeyPath_, LuaPackage::kSecretKeyBytes, secret, error)) {
      unsigned char publicKey[crypto_sign_PUBLICKEYBYTES];
      if (crypto_sign_ed25519_sk_to_pk(publicKey, secret.data()) == 0) {
        std::vector<unsigned char> pub(publicKey, publicKey + sizeof(publicKey));
        WriteKeyFile(publicKeyPath_, pub, error);
      }
      sodium_memzero(secret.data(), secret.size());
    }
    error.clear();
  }
  return true;
}

bool PackageKeys::LoadEncryptionKey(std::vector<unsigned char>& out, std::string& error) const {
  return ReadKeyFile(encryptionKeyPath_, LuaPackage::kKeyBytes, out, error);
}

bool PackageKeys::LoadPublicKey(std::vector<unsigned char>& out, std::string& error) const {
  return ReadKeyFile(publicKeyPath_, LuaPackage::kPublicKeyBytes, out, error);
}

bool PackageKeys::LoadSecretKey(std::vector<unsigned char>& out, std::string& error) const {
  return ReadKeyFile(secretKeyPath_, LuaPackage::kSecretKeyBytes, out, error);
}

bool PackageKeys::HasSigningKey() const {
  std::error_code ec;
  return fs::exists(secretKeyPath_, ec);
}

bool PackageKeys::HasEncryptionKey() const {
  std::error_code ec;
  return fs::exists(encryptionKeyPath_, ec);
}

std::string PackageKeys::PublicKeyFingerprint() const {
  std::vector<unsigned char> pub;
  std::string error;
  if (!LoadPublicKey(pub, error)) return std::string();
  Sha256 hasher;
  hasher.Update(pub.data(), pub.size());
  return hasher.HexDigest().substr(0, 16);
}

}  // namespace hsf
