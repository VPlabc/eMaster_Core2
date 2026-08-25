#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace hsf {

struct DownloadResult {
  bool ok = false;
  long status_code = 0;
  int64_t bytes = 0;
  std::string error;
};

// Streams one URL straight to a file over libcurl.
//
// Separate from RestClient on purpose. RestClient accumulates the whole
// response in a std::string, which is exactly right for a JSON API and exactly
// wrong for a release archive -- an ARM board with 512 MB of RAM should not
// hold a 40 MB package in memory alongside the copy it is writing to disk.
// This writes as it receives, reports progress so the popup's bar means
// something, and can be cancelled mid-transfer.
class Downloader {
 public:
  // Called with (bytesReceived, totalBytes). totalBytes is 0 when the server
  // sends no Content-Length -- the UI shows an indeterminate bar rather than
  // dividing by zero.
  using ProgressCallback = std::function<void(int64_t, int64_t)>;

  // Downloads `url` to `destinationPath`, creating parent directories as
  // needed and replacing anything already there. On failure the partial file
  // is removed: a half-written archive left on disk is one restart away from
  // being mistaken for a complete one.
  //
  // `cancel`, when it flips true, aborts the transfer and reports "cancelled".
  static DownloadResult ToFile(const std::string& url, const std::string& destinationPath, int timeoutSeconds,
                               bool verifySsl, const std::atomic<bool>* cancel, ProgressCallback progress);
};

}  // namespace hsf
