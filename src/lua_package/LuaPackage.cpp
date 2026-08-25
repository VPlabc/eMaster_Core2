#include "hsf/lua_package/LuaPackage.h"

#include <sodium.h>

#include <cstring>

#include "hsf/lua_package/LuaCompiler.h"
#include "hsf/security/PasswordHash.h"
#include "hsf/update/Sha256.h"

using nlohmann::json;

namespace hsf {
namespace {

void AppendU16(std::vector<unsigned char>& out, uint16_t value) {
  out.push_back(static_cast<unsigned char>(value & 0xFF));
  out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
}

void AppendU32(std::vector<unsigned char>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<unsigned char>((value >> (i * 8)) & 0xFF));
}

void AppendU64(std::vector<unsigned char>& out, uint64_t value) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<unsigned char>((value >> (i * 8)) & 0xFF));
}

// Every read is bounds-checked against the buffer end. A package is untrusted
// input until its signature has been checked, and the header has to be parsed
// BEFORE that check in order to know what is being verified -- so this parsing
// is the one place a malformed file gets to touch, and it must not be able to
// read past the end.
bool ReadU16(const std::vector<unsigned char>& in, size_t& offset, uint16_t& out) {
  if (offset + 2 > in.size()) return false;
  out = static_cast<uint16_t>(in[offset]) | static_cast<uint16_t>(in[offset + 1]) << 8;
  offset += 2;
  return true;
}

bool ReadU32(const std::vector<unsigned char>& in, size_t& offset, uint32_t& out) {
  if (offset + 4 > in.size()) return false;
  out = 0;
  for (int i = 0; i < 4; ++i) out |= static_cast<uint32_t>(in[offset + i]) << (i * 8);
  offset += 4;
  return true;
}

bool ReadU64(const std::vector<unsigned char>& in, size_t& offset, uint64_t& out) {
  if (offset + 8 > in.size()) return false;
  out = 0;
  for (int i = 0; i < 8; ++i) out |= static_cast<uint64_t>(in[offset + i]) << (i * 8);
  offset += 8;
  return true;
}

constexpr size_t kMagicBytes = 8;  // "HSFLUAP" + NUL
// A ceiling on the header so a claimed length cannot make us allocate wildly.
constexpr uint32_t kMaxHeaderBytes = 64 * 1024;
// And on the payload: a Lua application is measured in hundreds of kilobytes.
constexpr uint64_t kMaxPayloadBytes = 64ull * 1024 * 1024;

}  // namespace

nlohmann::json LuaPackage::ToJson(const Metadata& metadata) {
  return json{{"app_id", metadata.app_id},
              {"version", metadata.version},
              {"entry", metadata.entry},
              {"runtime_tag", metadata.runtime_tag},
              {"built_at", metadata.built_at},
              {"built_by", metadata.built_by},
              {"source_sha256", metadata.source_sha256},
              {"bytecode_sha256", metadata.bytecode_sha256},
              {"bytecode_bytes", metadata.bytecode_bytes},
              {"algorithm", metadata.algorithm},
              {"notes", metadata.notes}};
}

LuaPackage::BuildResult LuaPackage::Build(const std::vector<unsigned char>& bytecode,
                                          const Metadata& metadata,
                                          const std::vector<unsigned char>& encryptionKey,
                                          const std::vector<unsigned char>& secretKey) {
  BuildResult result;
  if (!PasswordHash::Initialise()) {
    result.error = "libsodium is not initialised";
    return result;
  }
  if (bytecode.empty()) {
    result.error = "there is no bytecode to package";
    return result;
  }
  if (encryptionKey.size() != kKeyBytes) {
    result.error = "the encryption key must be " + std::to_string(kKeyBytes) + " bytes";
    return result;
  }
  if (secretKey.size() != kSecretKeyBytes) {
    result.error = "the signing key must be " + std::to_string(kSecretKeyBytes) + " bytes";
    return result;
  }

  Metadata meta = metadata;
  meta.algorithm = "xchacha20poly1305-ietf";
  meta.bytecode_bytes = static_cast<int64_t>(bytecode.size());
  {
    Sha256 hasher;
    hasher.Update(bytecode.data(), bytecode.size());
    meta.bytecode_sha256 = hasher.HexDigest();
  }
  if (meta.runtime_tag.empty()) meta.runtime_tag = LuaCompiler::RuntimeTag();

  // A fresh random nonce per package. XChaCha20's nonce is 192 bits, which is
  // wide enough that random generation has no practical collision risk and
  // there is no counter to persist between builds -- the reason this family
  // was chosen over the 96-bit-nonce constructions.
  std::vector<unsigned char> nonce(kNonceBytes);
  randombytes_buf(nonce.data(), nonce.size());

  json header = ToJson(meta);
  header["format"] = kFormatVersion;
  header["nonce"] = [&nonce] {
    static const char* kHex = "0123456789abcdef";
    std::string hex;
    hex.reserve(nonce.size() * 2);
    for (unsigned char byte : nonce) {
      hex.push_back(kHex[byte >> 4]);
      hex.push_back(kHex[byte & 0x0F]);
    }
    return hex;
  }();
  const std::string headerText = header.dump();
  if (headerText.size() > kMaxHeaderBytes) {
    result.error = "package metadata is too large";
    return result;
  }

  std::vector<unsigned char> ciphertext(bytecode.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
  unsigned long long ciphertextLength = 0;
  // The header is passed as ADDITIONAL AUTHENTICATED DATA, so the AEAD tag
  // covers it too. Belt and braces alongside the signature: even a package
  // whose signature somehow verified could not have had its metadata swapped
  // for another package's without the decryption failing.
  crypto_aead_xchacha20poly1305_ietf_encrypt(
      ciphertext.data(), &ciphertextLength, bytecode.data(), bytecode.size(),
      reinterpret_cast<const unsigned char*>(headerText.data()), headerText.size(), nullptr, nonce.data(),
      encryptionKey.data());
  ciphertext.resize(static_cast<size_t>(ciphertextLength));

  std::vector<unsigned char> out;
  out.reserve(kMagicBytes + 2 + 4 + headerText.size() + 8 + ciphertext.size() + kSignatureBytes);
  out.insert(out.end(), kMagic, kMagic + 7);
  out.push_back(0);
  AppendU16(out, kFormatVersion);
  AppendU32(out, static_cast<uint32_t>(headerText.size()));
  out.insert(out.end(), headerText.begin(), headerText.end());
  AppendU64(out, static_cast<uint64_t>(ciphertext.size()));
  out.insert(out.end(), ciphertext.begin(), ciphertext.end());

  // Signed last, over everything written so far.
  unsigned char signature[crypto_sign_BYTES];
  unsigned long long signatureLength = 0;
  if (crypto_sign_detached(signature, &signatureLength, out.data(), out.size(), secretKey.data()) != 0) {
    result.error = "signing failed";
    return result;
  }
  out.insert(out.end(), signature, signature + signatureLength);

  result.ok = true;
  result.package = std::move(out);
  return result;
}

namespace {

// Shared by Open() and PeekMetadata(): validates the envelope and locates the
// pieces without touching any key.
struct Envelope {
  size_t headerOffset = 0;
  size_t headerLength = 0;
  size_t payloadOffset = 0;
  size_t payloadLength = 0;
  size_t signedLength = 0;  // bytes covered by the signature
  std::string headerText;
  json header;
};

bool ParseEnvelope(const std::vector<unsigned char>& package, Envelope& out, std::string& error) {
  if (package.size() < kMagicBytes + 2 + 4 + 8 + LuaPackage::kSignatureBytes) {
    error = "not a gateway Lua package (too short)";
    return false;
  }
  if (std::memcmp(package.data(), LuaPackage::kMagic, 7) != 0 || package[7] != 0) {
    error = "not a gateway Lua package (bad magic)";
    return false;
  }

  size_t offset = kMagicBytes;
  uint16_t format = 0;
  if (!ReadU16(package, offset, format)) {
    error = "truncated package";
    return false;
  }
  if (format != LuaPackage::kFormatVersion) {
    error = "package format version " + std::to_string(format) + " is not supported by this gateway";
    return false;
  }

  uint32_t headerLength = 0;
  if (!ReadU32(package, offset, headerLength)) {
    error = "truncated package";
    return false;
  }
  if (headerLength == 0 || headerLength > kMaxHeaderBytes || offset + headerLength > package.size()) {
    error = "package header length is implausible";
    return false;
  }
  out.headerOffset = offset;
  out.headerLength = headerLength;
  out.headerText.assign(reinterpret_cast<const char*>(package.data() + offset), headerLength);
  offset += headerLength;

  uint64_t payloadLength = 0;
  if (!ReadU64(package, offset, payloadLength)) {
    error = "truncated package";
    return false;
  }
  if (payloadLength == 0 || payloadLength > kMaxPayloadBytes) {
    error = "package payload length is implausible";
    return false;
  }
  // The remaining bytes must be exactly payload + signature. Checked with
  // subtraction against the buffer size rather than addition, which could
  // overflow on a crafted length.
  if (package.size() < offset + LuaPackage::kSignatureBytes ||
      package.size() - offset - LuaPackage::kSignatureBytes != payloadLength) {
    error = "package payload length does not match the file";
    return false;
  }
  out.payloadOffset = offset;
  out.payloadLength = static_cast<size_t>(payloadLength);
  out.signedLength = offset + static_cast<size_t>(payloadLength);

  try {
    out.header = json::parse(out.headerText);
  } catch (const std::exception&) {
    error = "package header is not valid JSON";
    return false;
  }
  if (!out.header.is_object()) {
    error = "package header is not a JSON object";
    return false;
  }
  return true;
}

LuaPackage::Metadata MetadataFrom(const json& header) {
  LuaPackage::Metadata meta;
  meta.app_id = header.value("app_id", std::string());
  meta.version = header.value("version", std::string());
  meta.entry = header.value("entry", std::string());
  meta.runtime_tag = header.value("runtime_tag", std::string());
  meta.built_at = header.value("built_at", std::string());
  meta.built_by = header.value("built_by", std::string());
  meta.source_sha256 = header.value("source_sha256", std::string());
  meta.bytecode_sha256 = header.value("bytecode_sha256", std::string());
  meta.bytecode_bytes = header.value("bytecode_bytes", static_cast<int64_t>(0));
  meta.algorithm = header.value("algorithm", std::string());
  meta.notes = header.value("notes", std::string());
  return meta;
}

bool HexToBytes(const std::string& hex, std::vector<unsigned char>& out) {
  if (hex.size() % 2 != 0) return false;
  out.clear();
  out.reserve(hex.size() / 2);
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int high = nibble(hex[i]);
    const int low = nibble(hex[i + 1]);
    if (high < 0 || low < 0) return false;
    out.push_back(static_cast<unsigned char>((high << 4) | low));
  }
  return true;
}

}  // namespace

bool LuaPackage::PeekMetadata(const std::vector<unsigned char>& package, Metadata& out,
                              std::string& error) {
  Envelope envelope;
  if (!ParseEnvelope(package, envelope, error)) return false;
  out = MetadataFrom(envelope.header);
  return true;
}

LuaPackage::OpenResult LuaPackage::Open(const std::vector<unsigned char>& package,
                                        const std::vector<unsigned char>& encryptionKey,
                                        const std::vector<unsigned char>& publicKey) {
  OpenResult result;
  if (!PasswordHash::Initialise()) {
    result.error = "libsodium is not initialised";
    return result;
  }
  if (encryptionKey.size() != kKeyBytes) {
    result.error = "the encryption key must be " + std::to_string(kKeyBytes) + " bytes";
    return result;
  }
  if (publicKey.size() != kPublicKeyBytes) {
    result.error = "the verification key must be " + std::to_string(kPublicKeyBytes) + " bytes";
    return result;
  }

  Envelope envelope;
  if (!ParseEnvelope(package, envelope, result.error)) return result;
  result.metadata = MetadataFrom(envelope.header);

  // Runtime compatibility BEFORE the signature, because a package built for a
  // different Lua is a legitimately-signed package that simply is not for this
  // machine, and saying so is more useful than "invalid signature".
  const std::string expected = LuaCompiler::RuntimeTag();
  if (!result.metadata.runtime_tag.empty() && result.metadata.runtime_tag != expected) {
    result.runtime_mismatch = true;
    result.error = "package was built for " + result.metadata.runtime_tag + ", this gateway runs " + expected;
    return result;
  }

  // Signature over everything before it.
  if (crypto_sign_verify_detached(package.data() + envelope.signedLength, package.data(),
                                  envelope.signedLength, publicKey.data()) != 0) {
    result.signature_failed = true;
    result.error = "signature does not verify -- this package was not built by this installation, or has "
                   "been modified";
    return result;
  }

  std::vector<unsigned char> nonce;
  if (!HexToBytes(envelope.header.value("nonce", std::string()), nonce) || nonce.size() != kNonceBytes) {
    result.error = "package header has no usable nonce";
    return result;
  }
  if (result.metadata.algorithm != "xchacha20poly1305-ietf") {
    result.error = "unsupported payload algorithm \"" + result.metadata.algorithm + "\"";
    return result;
  }

  std::vector<unsigned char> plaintext(envelope.payloadLength);
  unsigned long long plaintextLength = 0;
  if (crypto_aead_xchacha20poly1305_ietf_decrypt(
          plaintext.data(), &plaintextLength, nullptr, package.data() + envelope.payloadOffset,
          envelope.payloadLength, reinterpret_cast<const unsigned char*>(envelope.headerText.data()),
          envelope.headerText.size(), nonce.data(), encryptionKey.data()) != 0) {
    // Reaching here with a valid signature means the encryption key is the
    // wrong one -- the signature already ruled out tampering.
    result.error = "could not decrypt the package: wrong encryption key for this installation";
    return result;
  }
  plaintext.resize(static_cast<size_t>(plaintextLength));

  // The plaintext hash is redundant against the AEAD tag, which already
  // authenticates the ciphertext. It is checked anyway because it costs
  // microseconds and it is the one thing that would catch a mistake in THIS
  // code -- a length miscalculation that decrypted a correct package into
  // subtly wrong bytes would otherwise reach the Lua loader.
  Sha256 hasher;
  hasher.Update(plaintext.data(), plaintext.size());
  const std::string actual = hasher.HexDigest();
  if (!result.metadata.bytecode_sha256.empty() &&
      !Sha256::HexEquals(actual, result.metadata.bytecode_sha256)) {
    result.error = "decrypted payload does not match its recorded hash";
    return result;
  }

  // No "does this look like Lua bytecode?" check here. The payload is a
  // LuaBundle -- a module table whose first bytes are a count, not the Lua
  // signature -- and this class is deliberately agnostic about what it is
  // carrying. Validating the payload's shape belongs to LuaBundle::Parse,
  // which checks every module individually; a signature test at this layer
  // would have to be updated every time the payload gains a field.

  result.ok = true;
  result.bytecode = std::move(plaintext);
  return result;
}

}  // namespace hsf
