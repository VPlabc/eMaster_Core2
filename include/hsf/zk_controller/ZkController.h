#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hsf/zk_controller/C3Client.h"
#include "hsf/zk_controller/PullSdkClient.h"
// For EventCallback below: the parsed record is the whole point of that
// callback, so it is a real type here rather than a forward declaration.
#include "hsf/zk_controller/RTLogEvent.h"

namespace hsf {

// Card-swipe event handed to Lua's zk.onCard() callback. Field-equivalent to
// RTLogEvent. inOutStatus keeps its raw RTLog field name here; the Lua
// binding surfaces it as BOTH ReaderID and InOutStatus, since
// sdk-protocol-reference.md's GetRTLog buffer table (Attachment 7) defines
// field 5 as "Entry/Exit status: 0=entry, 1=exit, 2=none" -- i.e. which of
// a door's two readers (entry or exit) produced the swipe, which is exactly
// the reader index request/HSF_Machine_ZK_Controller_Lua_Integration.md's
// "Important: Verify ReaderID" section asked to confirm rather than assume.
struct ZkCardEvent {
  std::string cardData;
  int doorId = 0;
  int inOutStatus = 0;
  int verifyMode = 0;
  int eventType = 0;
  int64_t timestamp = 0;
};

// Combines AppState (connection lifecycle: connect/heartbeat/auto-reconnect)
// and RTLogPoller (500ms GetRTLog() polling) from ZKTecoProtocol into one
// Qt-free class with a single background thread -- see
// ZKTecoProtocol/src/appstate.* and src/rtlog/rtlog_poller.* for the
// original Qt (QObject/QTimer/signals) version this replaces. The
// connect/heartbeat/reconnect/poll *logic* is preserved verbatim (same
// intervals, same native PullSDK calls, same reconnect flow); only the
// QObject/QTimer scheduling is replaced with one thread doing a manual tick
// loop, per this integration's "do not create unnecessary threads"
// requirement -- RfidClient (src/RfidClient.cpp) is the model for owning a
// single background thread with connect/reconnect/callback logic.
//
// Thread safety: every callback fires from the background thread (RunLoop)
// ONLY, and never while any of this class's mutexes are held. Both halves of
// that matter:
//
//  - "RunLoop only": Connect()/Disconnect() change connection state from
//    whatever thread called them (a Lua script's, a web request's), but they
//    only *queue* the state change; RunLoop delivers it. Otherwise a Lua
//    script calling zk.connect() would re-enter LuaEngine's callback path on
//    a thread that already holds the Lua engine lock, and LuaEngine's
//    try_lock on an already-owned std::mutex is undefined behavior (it
//    deadlocks in practice on MSVC).
//  - "no mutex held": the callback is copied out from under callbackMutex_
//    and invoked after releasing it, so a callback that calls back into this
//    object (zk.disconnect() from inside onCard, say) can't self-deadlock.
//
// It's still NOT safe to touch a Lua state directly from inside them --
// LuaEngine queues instead (see its Zk* bindings), same as it already does
// for RfidClient/SerialPort.
// What the controller reports about its own I/O, as of the last RTLog poll.
//
// The panel does not answer "what is input 3 doing" on demand -- PullSDK has no
// read-input call. Two things arrive instead, both through GetRTLog:
//
//   * a door/alarm status record (recognised by field 4 == 255), carrying the
//     door-sensor state of every door as one byte each: 0 = no sensor
//     configured, 1 = closed, 2 = open;
//   * realtime events, of which type 220 (auxiliary input disconnected) and
//     221 (auxiliary input shorted) are the aux-input edges.
//
// So `doors` is a level and `aux_inputs` is the last edge seen per input --
// which is why `aux_input_seen` exists: an input nobody has triggered since the
// gateway started has no state at all, and reporting it as "off" would be a
// guess. See sdk-protocol-reference.md, Attachment 7 and Attached Table 6.
struct ZkIoState {
  // Door sensor per door number (1-based index into the vector). Values are the
  // raw DSS bytes above, kept raw rather than turned into bool so "no sensor
  // wired" stays distinguishable from "closed".
  std::vector<int> doors;
  std::vector<int> alarms;
  std::string status_time;
  bool status_seen = false;

  // Aux input number -> true when shorted (event 221), false when
  // disconnected (event 220).
  std::map<int, bool> aux_inputs;
  std::map<int, int64_t> aux_input_at;  // unix seconds of the last edge
  bool aux_input_seen = false;

  // From GetDeviceParam("LockCount,AuxOutCount,AuxInCount,ReaderCount"), read
  // once after each successful connect. 0 means "not read yet".
  int lock_count = 0;
  int aux_out_count = 0;
  int aux_in_count = 0;
  int reader_count = 0;
};

class ZkController {
 public:
  using ConnectionCallback = std::function<void(bool connected)>;
  using CardCallback = std::function<void(const ZkCardEvent&)>;
  using RawCallback = std::function<void(const std::string& rawBuffer)>;
  // EVERY parsed RTLog record, card or not -- door events, alarms, auxiliary
  // input edges, the lot. CardCallback deliberately drops anything whose cardNo
  // is "0", which is right for an access decision and wrong for a diagnostic
  // that exists to show what the panel is actually saying (Test Tool plan
  // section 43). Parsing stays here; nothing above this class re-implements it.
  using EventCallback = std::function<void(const RTLogEvent&)>;
  // Auxiliary input edge: (input number, shorted). Fires from the RunLoop
  // thread, same contract as the card callback.
  using AuxInputCallback = std::function<void(int input, bool shorted)>;

  ZkController();
  ~ZkController();

  // Starts/stops the single background thread that drives heartbeat,
  // reconnect, and RTLog polling. Call once at gateway startup/shutdown,
  // same as RfidClient::Start()/Stop() -- independent of Connect()/
  // StartRTLog() below, which can be called anytime after Start().
  void Start();
  void Stop();

  // Master switch for the built-in protocol driver. Disabling immediately
  // closes an existing session and blocks new zk.connect() calls; enabling
  // permits running Lua packages to connect again.
  void SetEnabled(bool enabled);
  bool IsEnabled() const;

  // Which protocol to reach the panel with: "auto" (default), "pullsdk" or
  // "c3". See the note on the backend members below for why this exists --
  // in short, PullSDK is a 32-bit Windows DLL and C3 is not, so this is what
  // gives a Linux gateway working doors. Set before Connect(); it is resolved
  // once per connection.
  void SetBackend(const std::string& backend);
  // "pullsdk" or "c3" -- what the last Connect() actually used.
  std::string BackendName() const;

  // Synchronous, like the original AppState::connectTcp -- does not retry;
  // auto-reconnect (below) only ever applies after a *successful* connect
  // that later drops.
  bool Connect(const std::string& ip, int port, int timeoutMs, const std::string& password);
  void Disconnect();
  bool IsConnected() const;
  bool IsReconnecting() const;

  // Default true, matching the original AppState's unconditional
  // reconnect-on-heartbeat-failure behavior; false disables it.
  void SetAutoReconnect(bool enable);

  void StartRTLog();
  void StopRTLog();
  bool IsRTLogRunning() const;

  std::string LastError() const;

  // --- output control (relays) --------------------------------------------
  //
  // All four go through ControlDevice; see PullSdkClient::ControlDevice for the
  // operation/parameter table. Every one of them is safe to call from any
  // thread (they take clientMutex_ like the poll loop does) and returns false
  // with LastError() set on an SDK error.
  //
  // `seconds`: 1..60 holds the relay closed for that long and the PANEL opens
  // it again on its own; 0 releases it now; 255 latches it ("normal open
  // state") until something releases it. Sub-second control is therefore only
  // possible as latch-then-release -- which is exactly how Beep() below works,
  // and why a beep costs two round trips to the panel.
  bool OpenDoor(int door, int seconds);
  bool SetAuxOutput(int auxOutput, int seconds);
  bool CancelAlarm();
  bool RestartDevice();
  bool SetNormallyOpen(int door, bool enable);

  // Raw escape hatch for an operation this class does not wrap.
  bool ControlDevice(int operationId, int param1, int param2, int param3, int param4,
                      const std::string& options = std::string());

  // Pulses an output for `durationMs`, by latching it (255) and releasing it
  // (0) after the wait. PullSDK has NO beeper command -- see the buzzer note in
  // the SmartLocker README -- so an audible signal is whatever is wired to that
  // relay, usually an auxiliary output.
  //
  // Blocks for count * (durationMs + gapMs). Called from a Lua script's own
  // thread, so that time is the script's to spend; nothing else in the gateway
  // waits on it.
  bool PulseOutput(int number, bool auxiliary, int durationMs);

  // Latch an output closed, or release it. This is the pair PulseOutput is
  // built from, exposed because a commissioning engineer needs to hold a relay
  // while they put a meter on it -- which a pulse cannot do.
  //
  // ON LEAVES IT CLOSED. There is no timer behind it: a door relay latched ON
  // is a door that stays unlocked until something calls this again, which is
  // why every caller that only wants a moment should use PulseOutput instead.
  bool SetOutput(int number, bool auxiliary, bool on);

  // Clears a latched output. Not simply "SetOutput(false)" spelled differently:
  // the command that releases a latch is NOT the inverse of the one that sets
  // it on this panel -- see the implementation, which was written against real
  // hardware after the obvious form turned out to succeed and do nothing.
  bool ReleaseOutput(int number, bool auxiliary);
  bool Beep(int number, bool auxiliary, int count, int durationMs, int gapMs);

  // --- parameters and I/O state -------------------------------------------

  // Raw GetDeviceParam/SetDeviceParam. `items` is comma-separated
  // ("LockCount,AuxOutCount"); the reply is the panel's own "Field=Value,..."
  // string, parsed for the caller into a map. Not const: the underlying SDK
  // call is not, and pretending otherwise would only mean a mutable member.
  std::map<std::string, std::string> GetParams(const std::string& items);
  bool SetParams(const std::string& itemValues);

  // Last known door/alarm/aux-input state -- see ZkIoState for what "last
  // known" means for each part of it.
  ZkIoState IoState() const;

  void SetConnectionCallback(ConnectionCallback callback);
  void SetCardCallback(CardCallback callback);
  void SetRawCallback(RawCallback callback);
  void SetAuxInputCallback(AuxInputCallback callback);
  void SetEventCallback(EventCallback callback);

 private:
  void RunLoop();
  void DoHeartbeat();
  void DoReconnectAttempt();
  void DoPoll();
  // Dedup guard, mirrors AppState::setConnected. Only *queues* the
  // notification (see the threading note above); DeliverConnectionEvents()
  // on the RunLoop thread is what actually calls the callback.
  void SetConnected(bool connected);
  void DeliverConnectionEvents();
  void SetLastError(const std::string& context, const std::string& detail);
  // Backend-aware; call with clientMutex_ held (see the definition).
  std::string BackendErrorText() const;

  // --- backend selection ---------------------------------------------------
  //
  // Two ways to reach the same panel:
  //
  //   PullSDK  plcommpro.dll, 32-bit Windows ONLY. Everywhere else it is
  //            PullSdkClient_stub and every call fails.
  //   C3       the panel's own protocol over TCP, works on every platform.
  //
  // Everything above this point -- the heartbeat, reconnect, RTLog polling,
  // the door/aux/alarm state, every zk.* Lua binding -- is identical for
  // both, because C3Client presents PullSdkClient's method surface and
  // formats its RT log into the same CSV RTLogParser already reads. The only
  // thing that varies is which object the six Backend* helpers below call.
  //
  // "auto" picks PullSDK where it can actually work and C3 otherwise, so a
  // Linux gateway gets real door control instead of NotSupportedOnThisPlatform.
  // SetBackend/BackendName are public, below.
  PullSdkClient client_;
  C3Client c3Client_;
  // Resolved once, at Connect(): switching mid-session would leave one backend
  // holding an open handle nothing will ever close.
  bool useC3_ = false;
  std::string backendPreference_ = "auto";

  // The six operations ZkController needs from a backend. Thin by design --
  // anything cleverer here would be logic that differs between backends, and
  // there isn't any.
  bool BackendConnect(const std::string& connectionString);
  void BackendDisconnect();
  int BackendControlDevice(int operationId, int p1, int p2, int p3, int p4, const std::string& options);
  std::string BackendGetDeviceParam(const std::string& items, int bufferSize, int* retOut);
  int BackendSetDeviceParam(const std::string& itemValues);
  std::string BackendGetRTLog(int bufferSize, int* retOut);
  // Guards every client_ call: unlike the original single-threaded Qt
  // AppState (everything ran on one Qt event-loop thread), Connect()/
  // Disconnect() can now be called from a Lua/web-request thread
  // concurrently with RunLoop's own heartbeat/reconnect/poll calls into the
  // same PullSdkClient handle.
  mutable std::mutex clientMutex_;

  mutable std::mutex stateMutex_;
  std::string lastConnectionString_;
  std::string lastError_;

  std::atomic<bool> threadRunning_{false};
  std::thread thread_;

  std::atomic<bool> enabled_{true};
  std::atomic<bool> connected_{false};
  std::atomic<bool> reconnecting_{false};
  // Stamped the moment reconnecting_ flips to true (DoHeartbeat), so
  // RunLoop's "due for a reconnect attempt" check counts a fresh 5s from
  // the failure, not from whenever the background thread happened to
  // start -- mirrors QTimer::start() always beginning a new countdown from
  // now, which is what the original AppState relied on. Written only from
  // RunLoop's own thread (DoHeartbeat runs on it), so plain, not atomic.
  std::chrono::steady_clock::time_point reconnectingSince_;
  std::atomic<bool> autoReconnect_{true};
  std::atomic<bool> rtlogRunning_{false};

  std::mutex callbackMutex_;
  ConnectionCallback connectionCallback_;
  CardCallback cardCallback_;
  RawCallback rawCallback_;
  AuxInputCallback auxInputCallback_;
  EventCallback eventCallback_;

  // Written only from the RunLoop thread (DoPoll / the post-connect param
  // read), read from anywhere via IoState() -- hence its own mutex rather than
  // clientMutex_, which a long ControlDevice call can hold for a while.
  mutable std::mutex ioMutex_;
  ZkIoState io_;

  // Reads LockCount/AuxOutCount/AuxInCount/ReaderCount into io_. Called from
  // RunLoop after a successful connect, not from Connect() itself: Connect()
  // can be on a Lua thread, and one more SDK call there is one more thing
  // holding the script up.
  void RefreshCounts();
  std::atomic<bool> countsPending_{false};

  // Connection-state changes queued by SetConnected() (any thread) for
  // RunLoop to deliver. A queue rather than a single flag so a fast
  // down/up pair can't collapse into "nothing happened".
  std::mutex connectionEventMutex_;
  std::deque<bool> connectionEvents_;
};

}  // namespace hsf
