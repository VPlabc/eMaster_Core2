#include "hsf/update/Downloader.h"

#include <curl/curl.h>

#include <cstdio>
#include <filesystem>
#include <system_error>

#include "hsf/Logger.h"

namespace hsf {
namespace {

struct Sink {
  std::FILE* file = nullptr;
  int64_t written = 0;
  int64_t total = 0;
  const std::atomic<bool>* cancel = nullptr;
  Downloader::ProgressCallback progress;
  // Throttles the callback: a 40 MB download is ~2500 write callbacks at
  // 16 KB each, and pushing a WebSocket frame per callback would flood the
  // browser with frames it cannot paint anyway.
  int64_t lastReported = 0;
};

size_t WriteToFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* sink = static_cast<Sink*>(userdata);
  const size_t bytes = size * nmemb;
  if (sink->cancel && sink->cancel->load()) return 0;  // any short write aborts the transfer

  if (std::fwrite(ptr, 1, bytes, sink->file) != bytes) return 0;
  sink->written += static_cast<int64_t>(bytes);

  if (sink->progress && (sink->written - sink->lastReported >= 256 * 1024 || sink->written == sink->total)) {
    sink->lastReported = sink->written;
    sink->progress(sink->written, sink->total);
  }
  return bytes;
}

int ProgressMeta(void* userdata, curl_off_t downloadTotal, curl_off_t downloaded, curl_off_t, curl_off_t) {
  auto* sink = static_cast<Sink*>(userdata);
  if (downloadTotal > 0) sink->total = static_cast<int64_t>(downloadTotal);
  // Covers the case where the body has not started arriving yet -- the write
  // callback has nothing to report until then, but a cancel must still bite.
  (void)downloaded;
  return (sink->cancel && sink->cancel->load()) ? 1 : 0;
}

}  // namespace

DownloadResult Downloader::ToFile(const std::string& url, const std::string& destinationPath, int timeoutSeconds,
                                   bool verifySsl, const std::atomic<bool>* cancel, ProgressCallback progress) {
  DownloadResult result;
  if (url.empty()) {
    result.error = "no download url";
    return result;
  }

  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(destinationPath).parent_path(), ec);

  std::FILE* file = std::fopen(destinationPath.c_str(), "wb");
  if (!file) {
    result.error = "cannot write to " + destinationPath;
    return result;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    std::fclose(file);
    std::filesystem::remove(destinationPath, ec);
    result.error = "curl_easy_init failed";
    return result;
  }

  Sink sink;
  sink.file = file;
  sink.cancel = cancel;
  sink.progress = std::move(progress);

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToFile);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressMeta);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &sink);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  // Release assets on GitHub redirect to a storage host; more than a handful
  // of hops means someone is looping us.
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "hsf-gateway/" HSF_VERSION);
  // A whole-transfer timeout, unlike RestClient's: this one is minutes long by
  // nature, so the guard that matters is "stalled", not "slow".
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, static_cast<long>(timeoutSeconds));
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(timeoutSeconds));
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verifySsl ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verifySsl ? 2L : 0L);

  const CURLcode code = curl_easy_perform(curl);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status_code);
  curl_easy_cleanup(curl);
  std::fclose(file);

  const bool cancelled = cancel && cancel->load();
  if (code != CURLE_OK) {
    result.error = cancelled ? "cancelled" : curl_easy_strerror(code);
  } else if (result.status_code >= 400) {
    result.error = "server returned HTTP " + std::to_string(result.status_code);
  } else {
    result.ok = true;
    result.bytes = sink.written;
  }

  if (!result.ok) {
    std::filesystem::remove(destinationPath, ec);
    Logger::Instance().Error(LogCategory::System, "Update download failed: " + result.error);
  }
  return result;
}

}  // namespace hsf
