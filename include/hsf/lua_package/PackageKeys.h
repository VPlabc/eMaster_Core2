#pragma once

#include <string>
#include <vector>

namespace hsf {

// Key material for Lua production packages (request/AdvanceUpdate.md section
// 2.8, which asks that the chosen model and its assumptions be documented
// rather than assumed).
//
// THREE KEYS, TWO OF WHICH LIVE ON THE DEVICE.
//
//   lua_signing.key   Ed25519 SECRET key (64 bytes). Signs packages. Present
//                     only where packages are BUILT. On a gateway that merely
//                     runs them it should be absent, and the gateway never
//                     needs it -- Deploy is the only operation that does.
//   lua_signing.pub   Ed25519 PUBLIC key (32 bytes). Verifies packages. On
//                     every device. Not secret.
//   lua_package.key   XChaCha20-Poly1305 symmetric key (32 bytes). Encrypts
//                     and decrypts. Needed on every device that runs a
//                     package AND wherever they are built.
//
// WHAT THIS MODEL DOES AND DOES NOT GIVE YOU.
//
//   The symmetric key has to be on the device, because the device decrypts
//   without a human present. Anyone with root there can read it. So encryption
//   protects the artifact everywhere the key is NOT -- a build server, a
//   release bucket, a backup, an email attachment, a stolen SD card imaged
//   without the running system -- and protects it not at all against someone
//   already inside the gateway. Stating that plainly is the point; a scheme
//   that claimed otherwise would be lying about where its security comes from.
//
//   The signature is the durable control. The secret key never has to leave
//   the machine that builds packages, so "only code we built runs here" holds
//   even against an attacker who has extracted the encryption key from a
//   device. That is why a package with a bad signature is refused
//   unconditionally, while a package that merely fails to decrypt reports a
//   configuration problem.
//
// SCOPE OF THE SYMMETRIC KEY is the installer's choice, and the trade is real:
// one key shared across a fleet means one package is deployable everywhere and
// one compromised device exposes every artifact; a per-device key means an
// artifact must be built per device and a compromise is contained. This class
// supports both by saying nothing about it -- the key is simply a file, and
// whoever provisions it decides how widely the same bytes are copied.
// docs/lua-packaging.md walks through both.
class PackageKeys {
 public:
  // Paths are resolved relative to `dataDir` (the config directory) when not
  // absolute -- the same rule as logging.definitions_path and the OTA public
  // key.
  explicit PackageKeys(std::string dataDir);

  // Creates any key that does not exist yet, with 0600 permissions where the
  // platform has them. Returns false only on an unwritable directory.
  //
  // `generatedSigningKey` is set when a signing keypair was created by this
  // call, so the caller can say so loudly once -- a new keypair invalidates
  // every package built with the old one, and that should never happen
  // silently.
  bool EnsureExists(bool wantSigningKey, bool& generatedSigningKey, std::string& error);

  bool LoadEncryptionKey(std::vector<unsigned char>& out, std::string& error) const;
  bool LoadPublicKey(std::vector<unsigned char>& out, std::string& error) const;
  bool LoadSecretKey(std::vector<unsigned char>& out, std::string& error) const;

  bool HasSigningKey() const;
  bool HasEncryptionKey() const;

  // Fingerprint of the public key -- first 8 bytes of its SHA-256, hex. Shown
  // in the UI so an operator can tell at a glance whether two installations
  // trust the same builder, without handling the key itself.
  std::string PublicKeyFingerprint() const;

  std::string EncryptionKeyPath() const { return encryptionKeyPath_; }
  std::string PublicKeyPath() const { return publicKeyPath_; }
  std::string SecretKeyPath() const { return secretKeyPath_; }

 private:
  bool WriteKeyFile(const std::string& path, const std::vector<unsigned char>& key,
                    std::string& error) const;
  bool ReadKeyFile(const std::string& path, size_t expectedBytes, std::vector<unsigned char>& out,
                   std::string& error) const;

  std::string dataDir_;
  std::string encryptionKeyPath_;
  std::string publicKeyPath_;
  std::string secretKeyPath_;
};

}  // namespace hsf
