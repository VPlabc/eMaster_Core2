#pragma once

#include <string>

namespace hsf {

// Detached-signature check over an update manifest.
//
// WHAT IS SIGNED. Not the archive, and not the whole manifest: the signed
// message is the three fields that identify one specific build, newline
// separated and in this fixed order --
//
//     <version>\n<platform>\n<sha256-hex>\n
//
// Signing the digest instead of the multi-megabyte archive keeps verification
// a fixed-cost operation on a device that may be an ARM board, and the digest
// still binds the archive byte for byte. Version and platform are in there
// because a signature over a bare digest is replayable: an attacker who can
// answer the update check could otherwise serve a genuinely-signed OLDER
// package and roll the gateway back to a version with a known hole. With them
// covered, a signature only ever authorises the exact (version, platform,
// bytes) triple the pipeline produced -- and UpdateManager separately refuses
// anything that is not strictly newer than what is running.
//
// The public key is a PEM file shipped beside the config database; the private
// half never leaves the release pipeline's secret store. See docs/ota-update.md.
//
// Two implementations, selected in CMakeLists.txt exactly like MqClient and the
// ZK PullSDK: the real one over OpenSSL when it is available, and a stub that
// fails every call with a clear message when it is not. Failing closed is the
// only honest stub here -- "no crypto compiled in" must never read as "the
// signature was fine".
class SignatureVerifier {
 public:
  // False (with `error` set) on a missing/unreadable key, a malformed
  // signature, or a signature that does not match. Never throws.
  //
  // `signatureBase64` is the detached signature as the manifest carries it.
  static bool Verify(const std::string& publicKeyPemPath, const std::string& signedMessage,
                     const std::string& signatureBase64, std::string& error);

  // Whether this build can verify anything at all. UpdateManager checks it up
  // front so a gateway that cannot verify says so at check time, instead of
  // after downloading 40 MB.
  static bool Available();

  // Human-readable backend name for the status endpoint ("OpenSSL 3.x" /
  // "not compiled in").
  static std::string Backend();

  // The exact message Verify() expects, built from manifest fields. Shared
  // with the packaging script through docs/ota-update.md -- if this format
  // changes, scripts/make-manifest.sh changes with it.
  static std::string BuildSignedMessage(const std::string& version, const std::string& platform,
                                        const std::string& sha256Hex);
};

}  // namespace hsf
