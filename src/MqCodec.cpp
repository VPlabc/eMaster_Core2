// Envelope v1 codec for MqClient, per qt-mq-lab's RabbitMQ.md:
//
//   {"id":"<uuid, no braces>","ts":"<ISO-8601 with ms, UTC>","v":1,"body":"<text>"}
//
// Its own translation unit because it has no dependency on AMQP-CPP -- which
// means it still works in a build with HSF_ENABLE_MQ off (a script can format a
// payload even where it cannot send one), and it can be exercised without a
// broker, the same split the reference app makes between mq_codec and
// mq_service.

#include <cstdio>
#include <ctime>
#include <random>
#include <string>

#include <nlohmann/json.hpp>

#include "hsf/MqClient.h"

namespace hsf {

namespace {

// ISO-8601 with milliseconds in UTC, e.g. "2026-08-12T09:15:03.123Z".
std::string IsoTimestampUtcMs() {
  using namespace std::chrono;
  const auto now = system_clock::now();
  const auto whole = time_point_cast<seconds>(now);
  const auto millis = duration_cast<milliseconds>(now - whole).count();
  const std::time_t raw = system_clock::to_time_t(whole);

  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &raw);
#else
  gmtime_r(&raw, &utc);
#endif

  char buffer[40] = {};
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900, utc.tm_mon + 1,
                utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, static_cast<int>(millis));
  return buffer;
}

// The timestamp must parse AND be UTC. A local-time stamp names a different
// instant, so accepting one silently corrupts every comparison made downstream
// -- hence the zone is required, not assumed.
bool LooksLikeUtcIsoMs(const std::string& text) {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, millis = 0;
  char zone[8] = {};
  const int fields = std::sscanf(text.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d.%3d%7s", &year, &month, &day, &hour, &minute,
                                 &second, &millis, zone);
  if (fields != 8) return false;
  if (month < 1 || month > 12 || day < 1 || day > 31) return false;
  if (hour > 23 || minute > 59 || second > 60) return false;

  const std::string suffix(zone);
  return suffix == "Z" || suffix == "z" || suffix == "+00:00" || suffix == "+0000";
}

// UUID v4 text without braces. Message ids only have to be unique, not
// unguessable -- but a fixed seed would repeat them across restarts and make
// deduplication upstream meaningless, hence random_device.
std::string MakeUuid() {
  static thread_local std::mt19937_64 engine(std::random_device{}());
  std::uniform_int_distribution<uint64_t> dist;

  uint64_t high = dist(engine);
  uint64_t low = dist(engine);

  // Version 4, variant 1 -- the bits that mark it as randomly generated.
  high = (high & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
  low = (low & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;

  char buffer[40] = {};
  std::snprintf(buffer, sizeof(buffer), "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(high >> 32),
                static_cast<unsigned>((high >> 16) & 0xFFFF), static_cast<unsigned>(high & 0xFFFF),
                static_cast<unsigned>((low >> 48) & 0xFFFF),
                static_cast<unsigned long long>(low & 0xFFFFFFFFFFFFull));
  return buffer;
}

}  // namespace

std::string MqClient::EncodeEnvelope(const std::string& body) {
  nlohmann::json doc;
  doc["id"] = MakeUuid();
  doc["ts"] = IsoTimestampUtcMs();
  doc["v"] = 1;
  doc["body"] = body;
  return doc.dump();
}

bool MqClient::DecodeEnvelope(const std::string& payload, MqMessage& out, std::string& error) {
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(payload);
  } catch (const std::exception& e) {
    error = std::string("not JSON: ") + e.what();
    return false;
  }

  if (!doc.is_object()) {
    error = "not a JSON object";
    return false;
  }

  // Every one of these must hold. A near-miss is not a v1 envelope, and
  // guessing at its intent is how a format change becomes silent corruption.
  if (!doc.contains("id") || !doc["id"].is_string() || doc["id"].get<std::string>().empty()) {
    error = "missing or empty \"id\"";
    return false;
  }
  if (!doc.contains("ts") || !doc["ts"].is_string() || !LooksLikeUtcIsoMs(doc["ts"].get<std::string>())) {
    error = "\"ts\" is not an ISO-8601 UTC timestamp with milliseconds";
    return false;
  }
  if (!doc.contains("v") || !doc["v"].is_number_integer() || doc["v"].get<int>() != 1) {
    error = "\"v\" is not 1";
    return false;
  }
  if (!doc.contains("body") || !doc["body"].is_string()) {
    error = "\"body\" is not a string";
    return false;
  }

  // Only the envelope's own fields are written: the caller has already filled
  // in exchange / routing key / delivery tag from the AMQP frame.
  out.id = doc["id"].get<std::string>();
  out.timestamp = doc["ts"].get<std::string>();
  out.version = doc["v"].get<int>();
  out.body = doc["body"].get<std::string>();
  return true;
}

}  // namespace hsf
