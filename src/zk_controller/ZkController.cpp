#include "hsf/zk_controller/ZkController.h"

#include <cstdio>
#include <ctime>

#include "hsf/Logger.h"
#include "hsf/zk_controller/RTLogParser.h"

namespace hsf {
namespace {

constexpr int kTickMs = 50;
constexpr int kHeartbeatIntervalMs = 5000;
constexpr int kReconnectIntervalMs = 5000;
constexpr int kPollIntervalMs = 500;
constexpr int kRTLogBufferSize = 256;
constexpr int kHeartbeatBufferSize = 256;

// Same connection-string shape as AppState::buildTcpConnectionString
// (ZKTecoProtocol/src/appstate.cpp) -- RS485 isn't part of this
// integration's scope (spec only asks for connect(ip, port, timeout,
// password)), so only the TCP builder is ported.
std::string BuildTcpConnectionString(const std::string& ip, int port, int timeoutMs, const std::string& password) {
  return "protocol=TCP,ipaddress=" + ip + ",port=" + std::to_string(port) + ",timeout=" +
         std::to_string(timeoutMs) + ",passwd=" + password;
}

// RTLog's "time" field is a plain "yyyy-MM-dd HH:mm:ss" string (not
// PullSDK's bespoke DateTime epoch -- that's only for the ~DateTime device
// param, see fastRead.md). Interpreted as local time, same ambiguity the
// original QDateTime::fromString(...) call had (no explicit UTC spec).
int64_t ParseRTLogTime(const std::string& text) {
  int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
  if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
    return 0;
  }
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
  tm.tm_isdst = -1;
  std::time_t t = std::mktime(&tm);
  return t == static_cast<std::time_t>(-1) ? 0 : static_cast<int64_t>(t);
}

// The panel answers GetDeviceParam with its own "Field=Value,Field=Value"
// string. Same shape parseDeviceParams() handles in
// ZKTecoProtocol/src/deviceparam, and the buffer is NUL-padded to the size we
// asked for, so the trailing zeros have to go before splitting.
std::map<std::string, std::string> ParseDeviceParams(const std::string& buffer) {
  std::map<std::string, std::string> out;

  std::string text = buffer;
  size_t nul = text.find('\0');
  if (nul != std::string::npos) text.resize(nul);

  size_t position = 0;
  while (position < text.size()) {
    size_t comma = text.find(',', position);
    std::string pair = text.substr(position, comma == std::string::npos ? std::string::npos : comma - position);
    position = comma == std::string::npos ? text.size() : comma + 1;

    size_t equals = pair.find('=');
    if (equals == std::string::npos) continue;

    std::string key = pair.substr(0, equals);
    std::string value = pair.substr(equals + 1);

    // Panels pad and occasionally wrap; a stray \r\n in a value would break
    // every numeric conversion downstream.
    while (!key.empty() && (key.front() == '\r' || key.front() == '\n' || key.front() == ' ')) key.erase(key.begin());
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) value.pop_back();

    if (!key.empty()) out[key] = value;
  }

  return out;
}

int ParamInt(const std::map<std::string, std::string>& params, const std::string& key) {
  auto it = params.find(key);
  if (it == params.end()) return 0;
  try {
    return std::stoi(it->second);
  } catch (const std::exception&) {
    return 0;
  }
}

// Door/alarm status arrives as one 32-bit value holding four bytes, one per
// door, door 1 in the LOW byte (sdk-protocol-reference.md, Attachment 7, note
// 2: 0x01020001 = door1 closed, door2 no sensor, door3 open, door4 closed).
std::vector<int> UnpackDoorBytes(uint32_t packed) {
  std::vector<int> out;
  out.reserve(4);
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<int>((packed >> (8 * i)) & 0xFF));
  }
  return out;
}

}  // namespace

ZkController::ZkController() = default;

ZkController::~ZkController() { Stop(); }

// --- backend selection --------------------------------------------------
//
// Six one-line forwarders. They exist so that everything above them -- the
// heartbeat, the reconnect loop, RTLog polling, the door and aux state, every
// zk.* binding -- is written once and works with either protocol.

void ZkController::SetBackend(const std::string& backend) {
  std::lock_guard<std::mutex> lock(clientMutex_);
  backendPreference_ = backend.empty() ? "auto" : backend;
}

std::string ZkController::BackendName() const { return useC3_ ? "c3" : "pullsdk"; }

bool ZkController::BackendConnect(const std::string& connectionString) {
  // Resolved here, once per connection, rather than at construction: the
  // preference can be changed from the Configuration page between connects.
  if (backendPreference_ == "c3") {
    useC3_ = true;
  } else if (backendPreference_ == "pullsdk") {
    useC3_ = false;
  } else {
#if defined(HSF_ENABLE_ZK)
    // PullSDK is compiled in, so it can actually load its DLL.
    useC3_ = false;
#else
    // Everywhere else PullSdkClient is the stub and would fail every call.
    // C3 needs nothing but a socket.
    useC3_ = true;
#endif
  }

  Logger::Instance().Info(LogCategory::Rfid,
                           std::string("ZK: connecting over ") + (useC3_ ? "C3/TCP" : "PullSDK"));
  return useC3_ ? c3Client_.Connect(connectionString) : client_.Connect(connectionString);
}

void ZkController::BackendDisconnect() {
  if (useC3_) {
    c3Client_.Disconnect();
  } else {
    client_.Disconnect();
  }
}

int ZkController::BackendControlDevice(int operationId, int p1, int p2, int p3, int p4,
                                        const std::string& options) {
  return useC3_ ? c3Client_.ControlDevice(operationId, p1, p2, p3, p4, options)
                : client_.ControlDevice(operationId, p1, p2, p3, p4, options);
}

std::string ZkController::BackendGetDeviceParam(const std::string& items, int bufferSize, int* retOut) {
  return useC3_ ? c3Client_.GetDeviceParam(items, bufferSize, retOut)
                : client_.GetDeviceParam(items, bufferSize, retOut);
}

int ZkController::BackendSetDeviceParam(const std::string& itemValues) {
  return useC3_ ? c3Client_.SetDeviceParam(itemValues) : client_.SetDeviceParam(itemValues);
}

std::string ZkController::BackendGetRTLog(int bufferSize, int* retOut) {
  return useC3_ ? c3Client_.GetRTLog(bufferSize, retOut) : client_.GetRTLog(bufferSize, retOut);
}

void ZkController::Start() {
  if (threadRunning_.load()) return;
  threadRunning_.store(true);
  thread_ = std::thread(&ZkController::RunLoop, this);
}

void ZkController::Stop() {
  if (!threadRunning_.load()) return;
  threadRunning_.store(false);
  if (thread_.joinable()) thread_.join();
  Disconnect();
}

void ZkController::SetEnabled(bool enabled) {
  const bool wasEnabled = enabled_.exchange(enabled);
  if (wasEnabled && !enabled) Disconnect();
}

bool ZkController::IsEnabled() const { return enabled_.load(); }

bool ZkController::Connect(const std::string& ip, int port, int timeoutMs, const std::string& password) {
  reconnecting_.store(false);
  std::string connStr = BuildTcpConnectionString(ip, port, timeoutMs, password);

  bool ok;
  std::string failure;
  {
    std::lock_guard<std::mutex> lock(clientMutex_);
    if (!enabled_.load()) {
      SetLastError("connect", "built-in ZK protocol driver is disabled");
      return false;
    }
    ok = BackendConnect(connStr);
    if (!ok) failure = BackendErrorText();
  }
  if (!ok) SetLastError("connect", failure);

  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    lastConnectionString_ = connStr;
  }
  SetConnected(ok);
  return ok;
}

void ZkController::Disconnect() {
  reconnecting_.store(false);
  std::lock_guard<std::mutex> lock(clientMutex_);
  BackendDisconnect();
  SetConnected(false);
}

bool ZkController::IsConnected() const { return connected_.load(); }

bool ZkController::IsReconnecting() const { return reconnecting_.load(); }

void ZkController::SetAutoReconnect(bool enable) { autoReconnect_.store(enable); }

void ZkController::StartRTLog() { rtlogRunning_.store(true); }

void ZkController::StopRTLog() { rtlogRunning_.store(false); }

bool ZkController::IsRTLogRunning() const { return rtlogRunning_.load(); }

std::string ZkController::LastError() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  return lastError_;
}

void ZkController::SetConnectionCallback(ConnectionCallback callback) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  connectionCallback_ = std::move(callback);
}

void ZkController::SetCardCallback(CardCallback callback) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  cardCallback_ = std::move(callback);
}

void ZkController::SetRawCallback(RawCallback callback) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  rawCallback_ = std::move(callback);
}

void ZkController::SetAuxInputCallback(AuxInputCallback callback) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  auxInputCallback_ = std::move(callback);
}

void ZkController::SetEventCallback(EventCallback callback) {
  std::lock_guard<std::mutex> lock(callbackMutex_);
  eventCallback_ = std::move(callback);
}

// --- output control --------------------------------------------------------

bool ZkController::ControlDevice(int operationId, int param1, int param2, int param3, int param4,
                                  const std::string& options) {
  if (!connected_.load()) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    lastError_ = "ControlDevice: the controller is not connected";
    return false;
  }

  int ret = 0;
  std::string failure;
  {
    // Held across the call: ControlDevice on an unreachable panel blocks for
    // the connection timeout, and the poll loop must not interleave another
    // request on the same handle while it does.
    std::lock_guard<std::mutex> lock(clientMutex_);
    ret = BackendControlDevice(operationId, param1, param2, param3, param4, options);
    if (ret < 0) failure = BackendErrorText();
  }

  if (ret < 0) {
    SetLastError("ControlDevice(" + std::to_string(operationId) + ")", failure);
    return false;
  }

  return true;
}

// param2 = 1 selects the door relay, 2 the auxiliary output; param3 is the
// hold in seconds (0 = release now, 255 = latch).
bool ZkController::OpenDoor(int door, int seconds) {
  return ControlDevice(1, door, 1, seconds, 0);
}

bool ZkController::SetAuxOutput(int auxOutput, int seconds) {
  return ControlDevice(1, auxOutput, 2, seconds, 0);
}

bool ZkController::CancelAlarm() { return ControlDevice(2, 0, 0, 0, 0); }

bool ZkController::RestartDevice() { return ControlDevice(3, 0, 0, 0, 0); }

bool ZkController::SetNormallyOpen(int door, bool enable) {
  return ControlDevice(4, door, enable ? 1 : 0, 0, 0);
}

bool ZkController::PulseOutput(int number, bool auxiliary, int durationMs) {
  int addressType = auxiliary ? 2 : 1;

  // 255 = "normal open state": the relay closes and STAYS closed until told
  // otherwise. That is what makes sub-second pulses possible -- param3 counts
  // whole seconds, so a 150 ms beep cannot be expressed as a duration.
  if (!ControlDevice(1, number, addressType, 255, 0)) return false;

  if (durationMs > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(durationMs));
  }

  // Released even if the caller is abandoning the operation: a latched relay is
  // a door left unlocked (or a buzzer that never stops). Through ReleaseOutput,
  // because the obvious duration-0 release is accepted-but-ineffective on this
  // panel -- see the note there. Every beep goes through here, so before that
  // was found each beep was leaving its relay closed.
  if (!ReleaseOutput(number, auxiliary)) {
    Logger::Instance().Error(LogCategory::System,
                              "ZK output " + std::to_string(number) +
                                  " stayed LATCHED -- the release command failed: " + LastError());
    return false;
  }

  return true;
}

bool ZkController::SetOutput(int number, bool auxiliary, bool on) {
  if (on) return ControlDevice(1, number, auxiliary ? 2 : 1, 255, 0);
  return ReleaseOutput(number, auxiliary);
}

// RELEASING A LATCHED OUTPUT IS NOT THE SAME COMMAND AS LATCHING IT, and this
// cost real hardware time to find out. Operation 1 with a duration of 0 -- the
// obvious inverse of the 255 that latched it -- is ACCEPTED by the panel and
// returns success while the relay stays closed. What actually clears the latch
// is operation 4 with param2 = 0: "cancel the normally-open state", the
// documented inverse of the normally-open state that 255 puts the relay into.
// Verified against a real controller (2 lock relays, firmware as shipped).
//
// WHY AUXILIARY OUTPUTS ARE TREATED DIFFERENTLY. Operation 4 addresses a DOOR
// and has no address-type parameter, so it cannot speak for an auxiliary
// output -- sending it with an aux number would cancel the normally-open state
// of the DOOR that happens to share that number, which is a different relay and
// possibly an unlocked one. Auxiliary outputs therefore keep the duration-0
// release, which is NOT verified on this hardware; if an aux-wired buzzer ever
// sticks on, this is the line to look at.
//
// Both forms are sent for a door relay rather than only the one that works
// here: the duration-0 form is harmless where it does nothing and correct on
// panels where it is honoured, and a relay left latched is a door left
// unlocked. Either succeeding counts as released.
bool ZkController::ReleaseOutput(int number, bool auxiliary) {
  if (auxiliary) return ControlDevice(1, number, 2, 0, 0);

  const bool zeroDuration = ControlDevice(1, number, 1, 0, 0);
  const bool cancelLatch = ControlDevice(4, number, 0, 0, 0);

  return zeroDuration || cancelLatch;
}

bool ZkController::Beep(int number, bool auxiliary, int count, int durationMs, int gapMs) {
  for (int i = 0; i < count; ++i) {
    if (!PulseOutput(number, auxiliary, durationMs)) return false;
    if (i + 1 < count && gapMs > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(gapMs));
    }
  }
  return true;
}

// --- parameters and I/O state ---------------------------------------------

std::map<std::string, std::string> ZkController::GetParams(const std::string& items) {
  std::string buffer;
  int ret = 0;
  {
    std::lock_guard<std::mutex> lock(clientMutex_);
    buffer = BackendGetDeviceParam(items, kHeartbeatBufferSize, &ret);
  }

  if (ret < 0) return {};
  return ParseDeviceParams(buffer);
}

bool ZkController::SetParams(const std::string& itemValues) {
  if (!connected_.load()) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    lastError_ = "SetDeviceParam: the controller is not connected";
    return false;
  }

  int ret = 0;
  std::string failure;
  {
    std::lock_guard<std::mutex> lock(clientMutex_);
    ret = BackendSetDeviceParam(itemValues);
    if (ret < 0) failure = BackendErrorText();
  }

  if (ret < 0) {
    SetLastError("SetDeviceParam", failure);
    return false;
  }

  return true;
}

ZkIoState ZkController::IoState() const {
  std::lock_guard<std::mutex> lock(ioMutex_);
  return io_;
}

void ZkController::RefreshCounts() {
  std::map<std::string, std::string> params = GetParams("LockCount,AuxOutCount,AuxInCount,ReaderCount");
  if (params.empty()) return;

  std::lock_guard<std::mutex> lock(ioMutex_);
  io_.lock_count = ParamInt(params, "LockCount");
  io_.aux_out_count = ParamInt(params, "AuxOutCount");
  io_.aux_in_count = ParamInt(params, "AuxInCount");
  io_.reader_count = ParamInt(params, "ReaderCount");
}

void ZkController::SetConnected(bool connected) {
  // Dedup guard, mirrors AppState::setConnected -- check-and-set under
  // stateMutex_ rather than a bare atomic compare, since Connect()/
  // Disconnect() (any thread) and RunLoop's heartbeat/reconnect (background
  // thread) can now race to call this, unlike the original single-threaded
  // Qt AppState.
  bool changed;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    changed = connected_.load() != connected;
    if (changed) connected_.store(connected);
  }
  if (!changed) return;

  // How many relays and inputs this panel has is worth knowing, but reading it
  // here would put another SDK round trip on whatever thread called
  // Connect() -- a Lua script's, usually. RunLoop picks this flag up instead.
  if (connected) countsPending_.store(true);

  // Queue only -- never invoke the callback on the caller's thread. See the
  // threading note in ZkController.h: Connect()/Disconnect() are routinely
  // called from a Lua script's thread, which already holds the Lua engine
  // lock, and delivering there re-enters LuaEngine on that same thread.
  std::lock_guard<std::mutex> lock(connectionEventMutex_);
  connectionEvents_.push_back(connected);
}

void ZkController::DeliverConnectionEvents() {
  for (;;) {
    bool connected;
    {
      std::lock_guard<std::mutex> lock(connectionEventMutex_);
      if (connectionEvents_.empty()) return;
      connected = connectionEvents_.front();
      connectionEvents_.pop_front();
    }

    ConnectionCallback callback;
    {
      std::lock_guard<std::mutex> lock(callbackMutex_);
      callback = connectionCallback_;
    }
    if (callback) callback(connected);
  }
}

// Reads whichever backend actually failed. MUST be called with clientMutex_
// held: C3Client is not thread-safe, and PullSDK's error is a process-wide
// static that the next call on any thread would overwrite.
//
// Before this existed every error path reported PullSdkClient::LastError()
// regardless of backend, so a C3 failure surfaced as "PullSDK error -1000
// (NotSupportedOnThisPlatform)" -- pointing the operator at a DLL that was
// never involved, and throwing away the C3 message that said what actually
// went wrong.
std::string ZkController::BackendErrorText() const {
  if (useC3_) {
    const std::string detail = c3Client_.LastError();
    return detail.empty() ? "the C3 backend gave no reason" : detail;
  }
  const PullError err = PullSdkClient::LastError();
  return "PullSDK error " + std::to_string(static_cast<int>(err)) + " (" + PullErrorToString(err) + ")";
}

void ZkController::SetLastError(const std::string& context, const std::string& detail) {
  std::lock_guard<std::mutex> lock(stateMutex_);
  lastError_ = context + " failed: " + detail;
}

void ZkController::DoHeartbeat() {
  int ret = 0;
  std::string failure;
  {
    std::lock_guard<std::mutex> lock(clientMutex_);
    BackendGetDeviceParam("~SerialNumber", kHeartbeatBufferSize, &ret);
    if (ret < 0) failure = BackendErrorText();
  }
  if (ret < 0) {
    SetLastError("heartbeat", failure);
    SetConnected(false);
    if (autoReconnect_.load()) {
      reconnectingSince_ = std::chrono::steady_clock::now();
      reconnecting_.store(true);
    }
  }
}

void ZkController::DoReconnectAttempt() {
  std::string connStr;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    connStr = lastConnectionString_;
  }

  bool ok;
  std::string failure;
  {
    std::lock_guard<std::mutex> lock(clientMutex_);
    BackendDisconnect();
    ok = BackendConnect(connStr);
    if (!ok) failure = BackendErrorText();
  }

  if (ok) {
    reconnecting_.store(false);
    SetConnected(true);
    // RTLog does not survive a reconnect at the device level (no backfill
    // in this integration, same limitation as fastread_sdk) -- polling
    // just resumes from here with whatever the controller has buffered
    // since reconnecting, per RunLoop's connected_-gated poll check below.
  } else {
    SetLastError("reconnect", failure);
  }
}

void ZkController::DoPoll() {
  int ret = 0;
  std::string buffer;
  {
    std::lock_guard<std::mutex> lock(clientMutex_);
    buffer = BackendGetRTLog(kRTLogBufferSize, &ret);
  }
  if (ret <= 0) return;

  // Copied out from under callbackMutex_ and invoked after releasing it, so
  // a callback that calls back into this object can't self-deadlock (see
  // the threading note in ZkController.h).
  RawCallback rawCallback;
  CardCallback cardCallback;
  AuxInputCallback auxCallback;
  EventCallback eventCallback;
  {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    rawCallback = rawCallback_;
    cardCallback = cardCallback_;
    auxCallback = auxInputCallback_;
    eventCallback = eventCallback_;
  }

  if (rawCallback) rawCallback(buffer.c_str());

  RTLogParseResult parsed = ParseRTLogBuffer(buffer);

  // Before any filtering: a diagnostic wants the panel's own words, including
  // the records the access path is right to ignore.
  if (eventCallback) {
    for (const RTLogEvent& event : parsed.events) eventCallback(event);
  }

  // Door-sensor levels. The panel sends these only when it has no events
  // queued (Attachment 7, note 1), so this is the "nothing is happening"
  // heartbeat of the door state -- and the only reading of an input the SDK
  // offers at all.
  for (const DoorAlarmStatus& status : parsed.statuses) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    io_.doors = UnpackDoorBytes(status.dssStatusPacked);
    io_.alarms = UnpackDoorBytes(status.alarmStatusPacked);
    io_.status_time = status.time;
    io_.status_seen = true;
  }

  // Auxiliary input edges. 220 = disconnected (open), 221 = shorted (closed);
  // the door field carries the input number for those two event types
  // (sdk-protocol-reference.md section 5.5 note on InAddr).
  for (const RTLogEvent& event : parsed.events) {
    if (event.eventType != 220 && event.eventType != 221) continue;

    bool shorted = event.eventType == 221;
    int input = event.doorNo;

    {
      std::lock_guard<std::mutex> lock(ioMutex_);
      io_.aux_inputs[input] = shorted;
      io_.aux_input_at[input] = ParseRTLogTime(event.time);
      io_.aux_input_seen = true;
    }

    if (auxCallback) auxCallback(input, shorted);
  }

  for (const RTLogEvent& event : parsed.events) {
    // rtlog_tab.cpp (RTLogTab::appendRow, the ZKTecoProtocol reference GUI)
    // filters these out before treating them as real card events --
    // door-open/alarm/password-fail records come through with a real
    // eventType but cardNo "0", not an actual card tap.
    if (event.cardNo == "0") continue;

    ZkCardEvent cardEvent;
    cardEvent.cardData = event.cardNo;
    cardEvent.doorId = event.doorNo;
    cardEvent.inOutStatus = event.inOutStatus;
    cardEvent.verifyMode = event.verifyMode;
    cardEvent.eventType = event.eventType;
    cardEvent.timestamp = ParseRTLogTime(event.time);

    if (cardCallback) cardCallback(cardEvent);
  }
}

void ZkController::RunLoop() {
  auto lastHeartbeat = std::chrono::steady_clock::now();
  auto lastPoll = std::chrono::steady_clock::now();

  while (threadRunning_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
    auto now = std::chrono::steady_clock::now();

    // Anything Connect()/Disconnect() queued from another thread, plus
    // whatever this loop's own heartbeat/reconnect queued last tick.
    DeliverConnectionEvents();

    if (!enabled_.load()) continue;

    if (connected_.load()) {
      // Once per successful connect, on this thread rather than the caller's.
      if (countsPending_.exchange(false)) {
        RefreshCounts();
      }
      if (now - lastHeartbeat >= std::chrono::milliseconds(kHeartbeatIntervalMs)) {
        lastHeartbeat = now;
        DoHeartbeat();
      }
      if (rtlogRunning_.load() && now - lastPoll >= std::chrono::milliseconds(kPollIntervalMs)) {
        lastPoll = now;
        DoPoll();
      }
    } else if (reconnecting_.load()) {
      // reconnectingSince_ is stamped fresh by DoHeartbeat() the moment
      // reconnecting_ became true, and again below after each failed
      // attempt -- so this always counts a full 5s from the *last*
      // failure/attempt, not from whenever RunLoop happened to start.
      if (now - reconnectingSince_ >= std::chrono::milliseconds(kReconnectIntervalMs)) {
        reconnectingSince_ = now;
        DoReconnectAttempt();
      }
    }
  }
}

}  // namespace hsf
