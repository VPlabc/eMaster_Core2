#include "hsf/zk_controller/C3Client.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

#include "hsf/Logger.h"

namespace hsf {
namespace {

// PullSDK connection strings are "key=value,key=value". Parsed leniently: an
// unknown key is ignored rather than fatal, because the string is shared with
// the PullSDK backend and may legitimately carry keys only that one uses.
std::string FieldFrom(const std::string& connectionString, const std::string& key) {
  const std::string needle = key + "=";
  size_t pos = 0;
  while (pos < connectionString.size()) {
    size_t end = connectionString.find(',', pos);
    if (end == std::string::npos) end = connectionString.size();
    const std::string token = connectionString.substr(pos, end - pos);
    // Case-insensitive key compare: PullSDK strings in the wild use both
    // "ipaddress" and "IPAddress".
    if (token.size() > needle.size()) {
      bool matches = true;
      for (size_t i = 0; i < needle.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(token[i])) !=
            std::tolower(static_cast<unsigned char>(needle[i]))) {
          matches = false;
          break;
        }
      }
      if (matches) return token.substr(needle.size());
    }
    pos = end + 1;
  }
  return std::string();
}

std::string ToString(const c3::Bytes& data) {
  return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

c3::Bytes ToBytes(const std::string& data) {
  return c3::Bytes(data.begin(), data.end());
}

}  // namespace

C3Client::~C3Client() { Disconnect(); }

bool C3Client::Connect(const std::string& connectionString) {
  Disconnect();
  lastError_.clear();

  host_ = FieldFrom(connectionString, "ipaddress");
  if (host_.empty()) host_ = FieldFrom(connectionString, "ip");
  const std::string portText = FieldFrom(connectionString, "port");
  const std::string timeoutText = FieldFrom(connectionString, "timeout");
  password_ = FieldFrom(connectionString, "passwd");

  if (host_.empty()) {
    lastError_ = "no ipaddress in the connection string";
    return false;
  }
  port_ = portText.empty() ? 4370 : std::atoi(portText.c_str());
  timeoutMs_ = timeoutText.empty() ? 2000 : std::atoi(timeoutText.c_str());
  if (timeoutMs_ < 500) timeoutMs_ = 500;

  if (!socket_.Connect(host_, port_, timeoutMs_)) {
    lastError_ = "cannot reach " + host_ + ":" + std::to_string(port_);
    return false;
  }
  carry_.clear();
  requestNr_ = 0;

  // Try a real session first, then fall back. Both are legitimate: the Qt
  // source records a real panel that rejects CONNECT_SESSION outright and only
  // answers session-less, so a failure here is a device difference, not an
  // error worth reporting to the operator.
  c3::Bytes reply;
  if (Transact(c3::EncodeConnectSession(password_), reply, timeoutMs_)) {
    const auto session = c3::DecodeConnectSession(reply);
    if (session.status == c3::ResponseStatus::kOk) {
      sessionLess_ = false;
      sessionId_ = session.session_id;
      requestNr_ = 0;
      connected_ = true;
      Logger::Instance().Info(LogCategory::Rfid,
                              "C3: session established with " + host_ + ":" + std::to_string(port_) +
                                  " (session id " + std::to_string(sessionId_) + ")");
      return true;
    }
  }

  // Session-less. The socket may hold a rejection reply we have not read, and
  // the panel may have closed it, so start from a clean connection rather than
  // reusing one in an unknown state.
  socket_.Close();
  carry_.clear();
  if (!socket_.Connect(host_, port_, timeoutMs_)) {
    lastError_ = "cannot reach " + host_ + ":" + std::to_string(port_) + " for a session-less connect";
    return false;
  }
  if (!Transact(c3::EncodeConnectSessionLess(password_), reply, timeoutMs_)) {
    lastError_ = "no reply to either CONNECT_SESSION or CONNECT_SESSION_LESS";
    socket_.Close();
    return false;
  }
  const auto generic = c3::DecodeGenericReply(reply);
  if (generic.status != c3::ResponseStatus::kOk) {
    lastError_ = generic.status == c3::ResponseStatus::kRejected
                     ? "the panel rejected the connection (wrong password?)"
                     : "the panel's connect reply was malformed";
    socket_.Close();
    return false;
  }

  sessionLess_ = true;
  sessionId_ = 0;
  connected_ = true;
  Logger::Instance().Info(LogCategory::Rfid, "C3: session-less connection to " + host_ + ":" +
                                                  std::to_string(port_));
  return true;
}

void C3Client::Disconnect() {
  if (connected_ && socket_.IsOpen()) {
    // Fire and forget: the reply is not required, and a panel that has already
    // gone away must not make shutdown block.
    const c3::Bytes frame = sessionLess_ ? c3::EncodeSessionLessDisconnect()
                                          : c3::EncodeDisconnect(sessionId_, NextRequestNr());
    socket_.Send(ToString(frame));
  }
  socket_.Close();
  connected_ = false;
  sessionId_ = 0;
  sessionLess_ = false;
  carry_.clear();
}

bool C3Client::IsConnected() const { return connected_ && socket_.IsOpen(); }

int32_t C3Client::NextRequestNr() {
  // Wraps in 16 bits because that is what the frame carries.
  requestNr_ = static_cast<int32_t>((requestNr_ + 1) & 0xFFFF);
  return requestNr_;
}

bool C3Client::ReadOneFrame(c3::Bytes& frame, int timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

  while (true) {
    // Is a whole frame already buffered from a previous read? A panel may
    // coalesce replies into one segment, and dropping the remainder would
    // desynchronise every later request.
    const size_t needed = c3::FrameSize(carry_);
    if (needed > 0 && carry_.size() >= needed) {
      frame.assign(carry_.begin(), carry_.begin() + needed);
      carry_.erase(carry_.begin(), carry_.begin() + needed);
      return true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return false;
    const int remaining =
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

    std::string chunk;
    if (!socket_.Receive(chunk, 4096, remaining > 0 ? remaining : 1)) return false;
    if (chunk.empty()) return false;  // peer closed
    carry_.insert(carry_.end(), chunk.begin(), chunk.end());

    // A runaway panel must not grow this without bound.
    if (carry_.size() > 1u * 1024 * 1024) {
      lastError_ = "the panel sent more than 1 MB without a complete frame";
      carry_.clear();
      return false;
    }
  }
}

bool C3Client::Transact(const c3::Bytes& request, c3::Bytes& reply, int timeoutMs) {
  if (!socket_.IsOpen()) {
    lastError_ = "not connected";
    return false;
  }
  if (!socket_.Send(ToString(request))) {
    lastError_ = "failed to send to the panel";
    connected_ = false;
    return false;
  }
  if (!ReadOneFrame(reply, timeoutMs)) {
    if (lastError_.empty()) lastError_ = "the panel did not reply within " + std::to_string(timeoutMs) + " ms";
    return false;
  }
  return true;
}

std::string C3Client::GetDeviceParam(const std::string& items, int /*bufferSize*/, int* retOut) {
  if (retOut) *retOut = -1;
  if (!IsConnected()) {
    lastError_ = "not connected";
    return std::string();
  }

  std::vector<std::string> names;
  size_t pos = 0;
  while (pos <= items.size()) {
    size_t end = items.find(',', pos);
    if (end == std::string::npos) end = items.size();
    if (end > pos) names.push_back(items.substr(pos, end - pos));
    if (end == items.size()) break;
    pos = end + 1;
  }

  c3::Bytes reply;
  if (!Transact(c3::EncodeGetParam(names, !sessionLess_, sessionId_, NextRequestNr()), reply, timeoutMs_)) {
    return std::string();
  }
  const auto decoded = c3::DecodeGetParam(reply, !sessionLess_);
  if (decoded.status != c3::ResponseStatus::kOk) {
    lastError_ = "GETPARAM was rejected by the panel";
    return std::string();
  }

  // Rebuilt as PullSDK's own "k=v,k=v" so callers cannot tell the backends
  // apart.
  std::string out;
  for (auto it = decoded.values.begin(); it != decoded.values.end(); ++it) {
    if (!out.empty()) out += ',';
    out += it.key() + "=" + it.value().get<std::string>();
  }
  if (retOut) *retOut = static_cast<int>(out.size());
  return out;
}

int C3Client::SetDeviceParam(const std::string& /*itemValues*/) {
  // Honest failure. The ported command set has no SETPARAM, and returning
  // success would make the Configuration page report a write that never
  // happened.
  lastError_ = "SetDeviceParam is not supported by the C3 backend (no SETPARAM in this command set)";
  return -1;
}

int C3Client::ControlDevice(int operationId, int param1, int param2, int param3, int /*param4*/,
                            const std::string& /*options*/) {
  if (!IsConnected()) {
    lastError_ = "not connected";
    return -1;
  }
  // PullSDK's operation ids and C3's are the same table (1 output, 2 cancel
  // alarm, 3 restart, 4 enable/disable normal open), so this is a cast rather
  // than a mapping -- but it is validated, because an out-of-range value would
  // otherwise be sent to a device that opens doors.
  if (operationId < 1 || operationId > 4) {
    lastError_ = "unknown control operation " + std::to_string(operationId);
    return -1;
  }

  const c3::Bytes request = c3::EncodeControl(
      static_cast<c3::ControlOperation>(operationId), static_cast<uint8_t>(param1),
      static_cast<uint8_t>(param2), static_cast<uint8_t>(param3), !sessionLess_, sessionId_,
      NextRequestNr());

  c3::Bytes reply;
  if (!Transact(request, reply, timeoutMs_)) return -1;

  const auto generic = c3::DecodeGenericReply(reply);
  if (generic.status != c3::ResponseStatus::kOk) {
    lastError_ = generic.status == c3::ResponseStatus::kRejected
                     ? "the panel rejected the control command"
                     : "the panel's control reply was malformed";
    return -1;
  }
  return 0;  // PullSDK's success code
}

std::string C3Client::GetRTLog(int /*bufferSize*/, int* retOut) {
  if (retOut) *retOut = -1;
  if (!IsConnected()) {
    lastError_ = "not connected";
    return std::string();
  }

  std::string csv;

  if (!rtlogKeyValueMode_) {
    c3::Bytes reply;
    if (!Transact(c3::EncodeRtlogBinaryRequest(!sessionLess_, sessionId_, NextRequestNr()), reply,
                  timeoutMs_)) {
      return std::string();
    }
    const auto decoded = c3::DecodeRtlogBinary(reply, !sessionLess_);

    if (decoded.status == c3::RtLogStatus::kNotBinaryMode) {
      // The panel's way of saying it has no binary mode. Switch once and stay
      // switched rather than paying for a failed probe on every poll.
      rtlogKeyValueMode_ = true;
      Logger::Instance().Info(LogCategory::Rfid,
                              "C3: panel does not support binary RT log; using key/value mode");
    } else if (decoded.status != c3::RtLogStatus::kOk) {
      lastError_ = "RTLOG was rejected by the panel";
      return std::string();
    } else {
      for (const c3::RtLogRecord& record : decoded.records) {
        // PullSDK's own CSV, so RTLogParser needs no second format:
        //   alarm:  time,dss,alarm,0,255,0,verify
        //   event:  time,pin,cardNo,door,eventType,inOut,verify
        const std::string time = c3::FormatC3Time(record.time_second);
        if (record.kind == c3::RtLogRecordKind::kDoorAlarmStatus) {
          uint32_t dss = 0;
          uint32_t alarm = 0;
          for (int i = 3; i >= 0; --i) {
            dss = (dss << 8) | (i < static_cast<int>(record.dss_status.size()) ? record.dss_status[i] : 0);
            alarm = (alarm << 8) |
                    (i < static_cast<int>(record.alarm_status.size()) ? record.alarm_status[i] : 0);
          }
          csv += time + "," + std::to_string(dss) + "," + std::to_string(alarm) + ",0,255,0," +
                 std::to_string(record.verified) + "\r\n";
        } else {
          csv += time + "," + std::to_string(record.pin) + "," + std::to_string(record.card_no) + "," +
                 std::to_string(record.door_id) + "," + std::to_string(record.event_type) + "," +
                 std::to_string(record.in_out_state) + "," + std::to_string(record.verified) + "\r\n";
        }
      }
    }
  }

  if (rtlogKeyValueMode_) {
    c3::Bytes reply;
    if (!Transact(c3::EncodeRtlogKeyValueRequest(!sessionLess_, sessionId_, NextRequestNr()), reply,
                  timeoutMs_)) {
      return std::string();
    }
    const auto decoded = c3::DecodeRtlogKeyValue(reply, !sessionLess_);
    if (decoded.status != c3::ResponseStatus::kOk) {
      lastError_ = "RTLOG (key/value) was rejected by the panel";
      return std::string();
    }
    for (const nlohmann::json& record : decoded.records) {
      // The key/value shape names its fields, so map them onto the same CSV
      // positions. Names follow zkaccess-c3-py's own output.
      auto field = [&record](const char* key, const char* fallback = "0") {
        if (record.contains(key) && record[key].is_string()) return record[key].get<std::string>();
        return std::string(fallback);
      };
      csv += field("time", "") + "," + field("pin") + "," + field("cardno") + "," + field("door") + "," +
             field("eventtype") + "," + field("inoutstate") + "," + field("verified") + "\r\n";
    }
  }

  if (retOut) *retOut = static_cast<int>(csv.size());
  return csv;
}

}  // namespace hsf
