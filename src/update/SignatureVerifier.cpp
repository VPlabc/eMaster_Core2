#include "hsf/update/SignatureVerifier.h"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>
#include <openssl/pem.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace hsf {
namespace {

std::string OpenSslError() {
  const unsigned long code = ERR_get_error();
  if (code == 0) return "unknown OpenSSL error";
  char buffer[256] = {};
  ERR_error_string_n(code, buffer, sizeof(buffer));
  // Drain the rest so the next call doesn't report a stale error.
  while (ERR_get_error() != 0) {
  }
  return buffer;
}

bool Base64Decode(const std::string& text, std::vector<unsigned char>& out) {
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };

  int accumulator = 0;
  int bits = 0;
  out.clear();
  for (char c : text) {
    if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
    if (c == '=') break;
    const int v = value(c);
    if (v < 0) return false;
    accumulator = (accumulator << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFF));
    }
  }
  return !out.empty();
}

// RAII for the two OpenSSL handles this file touches. Both verification paths
// have several failure exits; releasing by hand at each one is how a leak gets
// in on the fifth edit.
struct PkeyHandle {
  EVP_PKEY* key = nullptr;
  ~PkeyHandle() {
    if (key) EVP_PKEY_free(key);
  }
};

struct MdCtxHandle {
  EVP_MD_CTX* ctx = nullptr;
  ~MdCtxHandle() {
    if (ctx) EVP_MD_CTX_free(ctx);
  }
};

struct BioHandle {
  BIO* bio = nullptr;
  ~BioHandle() {
    if (bio) BIO_free(bio);
  }
};

}  // namespace

bool SignatureVerifier::Available() { return true; }

std::string SignatureVerifier::Backend() { return OPENSSL_VERSION_TEXT; }

std::string SignatureVerifier::BuildSignedMessage(const std::string& version, const std::string& platform,
                                                   const std::string& sha256Hex) {
  return version + "\n" + platform + "\n" + sha256Hex + "\n";
}

bool SignatureVerifier::Verify(const std::string& publicKeyPemPath, const std::string& signedMessage,
                                const std::string& signatureBase64, std::string& error) {
  std::vector<unsigned char> signature;
  if (!Base64Decode(signatureBase64, signature)) {
    error = "signature is not valid base64";
    return false;
  }

  // Read the PEM ourselves and hand OpenSSL a memory BIO, rather than calling
  // PEM_read_PUBKEY with a FILE*. On Windows, libcrypto lives in its own DLL
  // with its own C runtime, and a FILE* opened by this module is meaningless
  // over there -- OpenSSL detects it and aborts the process with
  // "OPENSSL_Uplink: no OPENSSL_Applink" unless the application links the
  // applink shim. A memory buffer crosses that boundary with no such problem,
  // costs nothing for a file this size, and keeps the POSIX and Windows paths
  // identical.
  std::string pem;
  {
    std::ifstream in(publicKeyPemPath, std::ios::binary);
    if (!in.is_open()) {
      error = "public key not readable at " + publicKeyPemPath;
      return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    pem = buffer.str();
  }
  if (pem.empty() || pem.size() > 64 * 1024) {
    error = "public key at " + publicKeyPemPath + " is empty or implausibly large";
    return false;
  }

  BioHandle bio;
  bio.bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (!bio.bio) {
    error = "out of memory reading the public key";
    return false;
  }
  PkeyHandle pkey;
  pkey.key = PEM_read_bio_PUBKEY(bio.bio, nullptr, nullptr, nullptr);
  if (!pkey.key) {
    error = "public key at " + publicKeyPemPath + " is not a PEM public key: " + OpenSslError();
    return false;
  }

  MdCtxHandle md;
  md.ctx = EVP_MD_CTX_new();
  if (!md.ctx) {
    error = "out of memory allocating a digest context";
    return false;
  }

  // Ed25519 (and Ed448) are one-shot algorithms that hash internally: OpenSSL
  // rejects them if a digest is named, and rejects everything else if one is
  // not. Which is why the branch is on key type rather than on a config flag.
  const int keyType = EVP_PKEY_base_id(pkey.key);
  const bool pureEdDSA = (keyType == EVP_PKEY_ED25519 || keyType == EVP_PKEY_ED448);

  if (EVP_DigestVerifyInit(md.ctx, nullptr, pureEdDSA ? nullptr : EVP_sha256(), nullptr, pkey.key) != 1) {
    error = "cannot initialise verification: " + OpenSslError();
    return false;
  }

  const unsigned char* message = reinterpret_cast<const unsigned char*>(signedMessage.data());
  int result;
  if (pureEdDSA) {
    result = EVP_DigestVerify(md.ctx, signature.data(), signature.size(), message, signedMessage.size());
  } else {
    if (EVP_DigestVerifyUpdate(md.ctx, message, signedMessage.size()) != 1) {
      error = "verification update failed: " + OpenSslError();
      return false;
    }
    result = EVP_DigestVerifyFinal(md.ctx, signature.data(), signature.size());
  }

  if (result != 1) {
    error = "signature does not match this version/platform/checksum";
    // Drain whatever OpenSSL queued so a later call reports its own error.
    while (ERR_get_error() != 0) {
    }
    return false;
  }
  return true;
}

}  // namespace hsf
