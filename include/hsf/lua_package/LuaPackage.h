#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace hsf {

// The production Lua artifact (request/AdvanceUpdate.md sections 2.6, 2.7,
// 2.9): compiled bytecode, encrypted, signed, with enough metadata in the
// clear to refuse it intelligently.
//
// ON-DISK LAYOUT. Fixed-size fields are little-endian.
//
//   offset  size          field
//   0       8             magic "HSFLUAP\0"
//   8       2             format version (currently 1)
//   10      4             header length H
//   14      H             header, JSON, PLAINTEXT (see below)
//   14+H    8             ciphertext length C
//   22+H    C             XChaCha20-Poly1305 ciphertext + 16-byte tag
//   22+H+C  64            Ed25519 signature over bytes [0, 22+H+C)
//
// The header is deliberately NOT encrypted. Deciding whether a package can run
// here -- right version, right runtime, right application -- must be possible
// before decrypting, so that a package built for a different Lua or a
// different machine is refused with a sentence rather than a MAC failure that
// could mean anything. It carries nothing secret: name, version, timestamps,
// the algorithm, the nonce, and the SHA-256 of the plaintext bytecode.
//
// The signature covers everything before it, header included, so the metadata
// cannot be edited to make a package claim a version it is not.
//
// WHAT THE ENCRYPTION IS AND IS NOT FOR -- section 2.8 asks for the threat
// model to be stated, so:
//
//   The gateway must decrypt this to run it, so the key is on the device. Any
//   attacker who reaches root on the device can read the key and decrypt the
//   package; encryption does NOT defend against that and no scheme that keeps
//   the machine able to boot unattended can. What it defends against is the
//   package in transit and at rest elsewhere -- on a build server, in a
//   release bucket, on a USB stick, in a backup, in an email -- where the key
//   is not. It raises the cost of casual extraction from "unzip it" to
//   "compromise a gateway first".
//
//   The SIGNATURE is the part that carries real weight. It is what makes
//   "only code we built runs here" true, and it holds even against someone who
//   has the encryption key, because the signing key never leaves the build
//   machine.
//
// WHY XChaCha20-Poly1305 AND NOT AES-256-GCM, which section 2.7 suggests:
// libsodium exposes AES-256-GCM only where the CPU has AES-NI, and
// crypto_aead_aes256gcm_is_available() is false on most ARM boards -- exactly
// the linux-arm64 target this is for. A cipher that silently is not there on
// the deployment hardware is worse than a different one that always is.
// XChaCha20-Poly1305 is constant-time in software everywhere, and its 192-bit
// nonce can be drawn at random with no counter to keep. The algorithm name is
// in the header, so adding AES-GCM later is a new value, not a new format.
class LuaPackage {
 public:
  static constexpr const char* kMagic = "HSFLUAP";
  static constexpr uint16_t kFormatVersion = 1;
  static constexpr size_t kKeyBytes = 32;      // crypto_aead_xchacha20poly1305_ietf_KEYBYTES
  static constexpr size_t kNonceBytes = 24;    // ..._NPUBBYTES
  static constexpr size_t kSignatureBytes = 64;
  static constexpr size_t kPublicKeyBytes = 32;
  static constexpr size_t kSecretKeyBytes = 64;

  struct Metadata {
    std::string app_id;        // which application this is, e.g. "smartlocker"
    std::string version;       // semver, drives rollback ordering
    std::string entry;         // original source path, for diagnostics
    std::string runtime_tag;   // LuaCompiler::RuntimeTag() at build time
    std::string built_at;      // ISO-8601 UTC
    std::string built_by;      // username that pressed Deploy
    std::string source_sha256; // hash of the ORIGINAL SOURCE, for traceability
    std::string bytecode_sha256;
    int64_t bytecode_bytes = 0;
    std::string algorithm = "xchacha20poly1305-ietf";
    std::string notes;
  };

  struct BuildResult {
    bool ok = false;
    std::vector<unsigned char> package;
    std::string error;
  };

  struct OpenResult {
    bool ok = false;
    std::vector<unsigned char> bytecode;  // plaintext, in memory only
    Metadata metadata;
    std::string error;
    // Distinguishes "this is not for us" from "this has been tampered with".
    // The first is a configuration mistake; the second is an attack, and the
    // two should not read the same in a log.
    bool signature_failed = false;
    bool runtime_mismatch = false;
  };

  // Encrypt + sign. `secretKey` is the 64-byte Ed25519 secret key.
  static BuildResult Build(const std::vector<unsigned char>& bytecode, const Metadata& metadata,
                           const std::vector<unsigned char>& encryptionKey,
                           const std::vector<unsigned char>& secretKey);

  // Verify + decrypt. Returns the bytecode in memory; NOTHING is written to
  // disk (section 2.7's "do not write decrypted bytecode back to persistent
  // storage").
  //
  // Order matters and is fixed: parse the header, check the runtime tag, VERIFY
  // THE SIGNATURE, then decrypt. Decrypting first would run the cipher over
  // attacker-chosen bytes before establishing that they came from us.
  static OpenResult Open(const std::vector<unsigned char>& package,
                         const std::vector<unsigned char>& encryptionKey,
                         const std::vector<unsigned char>& publicKey);

  // Header only: no key needed, no signature check. For listing packages in
  // the UI, where the alternative is decrypting every artifact to show a
  // table. Never use this to decide whether something may run.
  static bool PeekMetadata(const std::vector<unsigned char>& package, Metadata& out, std::string& error);

  static nlohmann::json ToJson(const Metadata& metadata);
};

}  // namespace hsf
