#include "hsf/update/SignatureVerifier.h"

namespace hsf {

// Built when OpenSSL is not available (see CMakeLists.txt, and the same
// pattern in MqClient_stub.cpp / PullSdkClient_stub.cpp).
//
// Fails closed, always. The alternative -- quietly treating "we cannot check
// the signature" as "the signature is fine" -- would turn a build option into
// a remote code execution path, on a device that is by definition reachable
// from the network it downloads updates over. A gateway built without OpenSSL
// can still be updated by hand; it just cannot be updated over the air unless
// update.require_signature is explicitly turned off.

bool SignatureVerifier::Available() { return false; }

std::string SignatureVerifier::Backend() { return "not compiled in (built without OpenSSL)"; }

std::string SignatureVerifier::BuildSignedMessage(const std::string& version, const std::string& platform,
                                                   const std::string& sha256Hex) {
  return version + "\n" + platform + "\n" + sha256Hex + "\n";
}

bool SignatureVerifier::Verify(const std::string&, const std::string&, const std::string&, std::string& error) {
  error =
      "this gateway was built without OpenSSL, so update signatures cannot be verified. "
      "Rebuild with OpenSSL available, or install the update manually.";
  return false;
}

}  // namespace hsf
