#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <string>

#include "hsf/ConfigManager.h"

namespace hsf {

struct RestResponse {
  bool ok = false;
  long status_code = 0;
  std::string body;
  std::string error;

  // Populated by Send() only -- the Test Tools page reports round-trip time
  // and shows the response headers, which the fire-and-forget callers above
  // have no use for.
  double elapsed_ms = 0.0;
  std::string response_headers;
};

// A free-form HTTP request, as composed on the Test Tools page. Deliberately
// carries everything explicitly rather than falling back to RestConfig: the
// point of the tool is to probe an arbitrary endpoint, not the configured one.
struct RestRequestSpec {
  std::string method = "GET";
  std::string url;
  std::map<std::string, std::string> headers;

  // A raw body and form fields are mutually exclusive; form_fields wins and
  // is sent as multipart/form-data (matching what the card readers post).
  std::string body;
  std::map<std::string, std::string> form_fields;

  int timeout_ms = 5000;
  bool verify_ssl = true;
};

// REST API client used to forward data (e.g. parsed citizen ID cards) to the
// external server. Configured from RestConfig (URL, API-Key, timeout,
// retry count, SSL enable) and usable both from C++ modules and from Lua
// scripts via the Rest.* bindings.
class RestClient {
 public:
  RestClient();
  ~RestClient();

  void Configure(const RestConfig& config);
  RestConfig GetConfig() const;

  // Convenience setters mirroring the Lua API (Rest.SetUrl / Rest.SetApiKey).
  void SetUrl(const std::string& url);
  void SetApiKey(const std::string& apiKey);

  RestResponse Get(const std::string& urlOverride = "");

  // Sends `fields` as multipart/form-data with the configured API-Key
  // header, retrying up to config.retry_count times on failure. The
  // urlOverride form posts to that exact URL instead of the configured one
  // (used for the Lua Rest.PostForm(path, data) convenience overload, which
  // joins it onto the configured base URL first).
  RestResponse PostForm(const std::map<std::string, std::string>& fields);
  RestResponse PostForm(const std::string& urlOverride, const std::map<std::string, std::string>& fields);

  bool LastRequestSucceeded() const { return lastSucceeded_.load(); }

  // Performs a one-shot GET using `config` directly (ignoring whatever this
  // client is currently Configure()'d with, and without touching that
  // state). Used by the Configuration page's "Test Connection" button so a
  // candidate URL/API-Key can be checked before saving it.
  RestResponse TestConnection(const RestConfig& config);

  // Free-form request for the Test Tools REST client. Static because it
  // borrows nothing from this client's configured state -- it exists so the
  // page can hit any endpoint, including the gateway's own.
  static RestResponse Send(const RestRequestSpec& spec);

 private:
  RestResponse ExecuteGet(const RestConfig& config, const std::string& url);
  RestResponse PostFormOnce(const std::string& url, const std::map<std::string, std::string>& fields);

  mutable std::mutex mutex_;
  RestConfig config_;
  std::atomic<bool> lastSucceeded_{false};
};

}  // namespace hsf
