#include "hsf/RestClient.h"

#include <curl/curl.h>

#include <chrono>
#include <thread>

#include "hsf/Logger.h"

namespace hsf {

namespace {
size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}
}  // namespace

RestClient::RestClient() { curl_global_init(CURL_GLOBAL_DEFAULT); }
RestClient::~RestClient() { curl_global_cleanup(); }

void RestClient::Configure(const RestConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = config;
}

RestConfig RestClient::GetConfig() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_;
}

void RestClient::SetUrl(const std::string& url) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_.url = url;
}

void RestClient::SetApiKey(const std::string& apiKey) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_.api_key = apiKey;
}

RestResponse RestClient::Get(const std::string& urlOverride) {
  RestConfig config = GetConfig();
  std::string url = urlOverride.empty() ? config.url : urlOverride;
  RestResponse response = ExecuteGet(config, url);
  lastSucceeded_.store(response.ok);
  return response;
}

RestResponse RestClient::TestConnection(const RestConfig& config) {
  return ExecuteGet(config, config.url);
}

RestResponse RestClient::Send(const RestRequestSpec& spec) {
  RestResponse response;
  if (spec.url.empty()) {
    response.error = "url is required";
    return response;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    response.error = "curl_easy_init failed";
    return response;
  }

  std::string body;
  std::string responseHeaders;
  struct curl_slist* headerList = nullptr;
  curl_mime* mime = nullptr;

  for (const auto& [name, value] : spec.headers) {
    if (name.empty()) continue;
    headerList = curl_slist_append(headerList, (name + ": " + value).c_str());
  }

  curl_easy_setopt(curl, CURLOPT_URL, spec.url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &responseHeaders);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(spec.timeout_ms));
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, spec.verify_ssl ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, spec.verify_ssl ? 2L : 0L);
  // Without this, a redirect returns the 30x itself and the tool looks broken.
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  std::string method = spec.method.empty() ? "GET" : spec.method;
  if (!spec.form_fields.empty()) {
    mime = curl_mime_init(curl);
    for (const auto& [name, value] : spec.form_fields) {
      curl_mimepart* part = curl_mime_addpart(mime);
      curl_mime_name(part, name.c_str());
      curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
    }
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    // CURLOPT_MIMEPOST implies POST; CUSTOMREQUEST still lets PUT/PATCH
    // carry a multipart body.
    if (method != "POST") curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  } else if (method == "GET") {
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  } else if (method == "HEAD") {
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  } else {
    // POSTFIELDS with an explicit size, so a body containing NULs survives.
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, spec.body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(spec.body.size()));
  }

  if (headerList) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

  CURLcode res = curl_easy_perform(curl);
  if (res == CURLE_OK) {
    long statusCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    response.status_code = statusCode;
    response.body = body;
    response.response_headers = responseHeaders;
    response.ok = statusCode >= 200 && statusCode < 400;
  } else {
    response.error = curl_easy_strerror(res);
  }

  double total = 0.0;
  curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &total);
  response.elapsed_ms = total * 1000.0;

  if (mime) curl_mime_free(mime);
  if (headerList) curl_slist_free_all(headerList);
  curl_easy_cleanup(curl);
  return response;
}

RestResponse RestClient::ExecuteGet(const RestConfig& config, const std::string& url) {
  RestResponse response;
  CURL* curl = curl_easy_init();
  if (!curl) {
    response.error = "curl_easy_init failed";
    return response;
  }

  std::string body;
  struct curl_slist* headers = nullptr;
  if (!config.api_key.empty()) {
    std::string header = "API-Key: " + config.api_key;
    headers = curl_slist_append(headers, header.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config.timeout_ms));
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, config.ssl_enable ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, config.ssl_enable ? 2L : 0L);

  CURLcode res = curl_easy_perform(curl);
  if (res == CURLE_OK) {
    long statusCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    response.status_code = statusCode;
    response.body = body;
    response.ok = statusCode >= 200 && statusCode < 300;
  } else {
    response.error = curl_easy_strerror(res);
  }

  if (headers) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return response;
}

RestResponse RestClient::PostFormOnce(const std::string& url, const std::map<std::string, std::string>& fields) {
  RestConfig config = GetConfig();
  RestResponse response;

  CURL* curl = curl_easy_init();
  if (!curl) {
    response.error = "curl_easy_init failed";
    return response;
  }

  curl_mime* mime = curl_mime_init(curl);
  for (const auto& [name, value] : fields) {
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, name.c_str());
    curl_mime_data(part, value.c_str(), CURL_ZERO_TERMINATED);
  }

  struct curl_slist* headers = nullptr;
  if (!config.api_key.empty()) {
    std::string header = "API-Key: " + config.api_key;
    headers = curl_slist_append(headers, header.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  }

  std::string body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(config.timeout_ms));
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, config.ssl_enable ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, config.ssl_enable ? 2L : 0L);

  CURLcode res = curl_easy_perform(curl);
  if (res == CURLE_OK) {
    long statusCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    response.status_code = statusCode;
    response.body = body;
    response.ok = statusCode >= 200 && statusCode < 300;
  } else {
    response.error = curl_easy_strerror(res);
  }

  if (headers) curl_slist_free_all(headers);
  curl_mime_free(mime);
  curl_easy_cleanup(curl);
  return response;
}

RestResponse RestClient::PostForm(const std::map<std::string, std::string>& fields) {
  return PostForm("", fields);
}

RestResponse RestClient::PostForm(const std::string& urlOverride, const std::map<std::string, std::string>& fields) {
  RestConfig config = GetConfig();
  std::string url = urlOverride.empty() ? config.url : urlOverride;
  RestResponse response;
  int attempts = config.retry_count < 1 ? 1 : config.retry_count;

  for (int attempt = 1; attempt <= attempts; ++attempt) {
    response = PostFormOnce(url, fields);
    if (response.ok) break;

    Logger::Instance().Warning(LogCategory::Rest, "PostForm attempt " + std::to_string(attempt) + "/" +
                                                        std::to_string(attempts) + " failed: " +
                                                        (response.error.empty() ? std::to_string(response.status_code)
                                                                                 : response.error));
    if (attempt < attempts) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
    }
  }

  lastSucceeded_.store(response.ok);
  return response;
}

}  // namespace hsf
