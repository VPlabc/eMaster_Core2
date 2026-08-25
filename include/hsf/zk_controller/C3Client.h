#pragma once

#include <cstdint>
#include <string>

#include "hsf/TcpSocket.h"
#include "hsf/zk_controller/C3Codec.h"

namespace hsf {

// A ZKTeco panel driver that speaks the C3 protocol over plain TCP, with the
// SAME method surface as PullSdkClient.
//
// WHY THE SHAPE IS COPIED RATHER THAN DESIGNED. ZkController already drives
// PullSdkClient through six methods, and RTLogParser already parses the exact
// CSV that PullSDK's GetRTLog returns. Presenting that same surface means the
// whole stack above -- ZkController's heartbeat and reconnect, the RTLog
// polling loop, the door/aux/alarm plumbing, every zk.* Lua binding, the Test
// Tool pages -- works unchanged, and the only new code is what actually
// differs: the wire protocol. A nicer, more "native" C3 API would have meant
// touching all of that to gain nothing.
//
// The one real translation is in GetRTLog: C3 hands back 16-byte binary
// records, and this formats them into PullSDK's
// "time,pin,cardNo,door,eventType,inOut,verify" CSV so RTLogParser needs no
// change. Doing it here rather than teaching the parser a second format keeps
// one definition of what an RTLog record means.
//
// NOT THREAD-SAFE, deliberately, exactly like PullSdkClient: ZkController
// already serialises every backend call behind clientMutex_, and a second lock
// underneath it would be redundant and a deadlock waiting to be written.
class C3Client {
 public:
  C3Client() = default;
  ~C3Client();

  C3Client(const C3Client&) = delete;
  C3Client& operator=(const C3Client&) = delete;

  // Accepts PullSDK's own connection-string format so the caller does not have
  // to care which backend it has:
  //   "protocol=TCP,ipaddress=10.0.0.237,port=4370,timeout=2000,passwd="
  bool Connect(const std::string& connectionString);
  void Disconnect();
  bool IsConnected() const;

  // "SerialNumber,LockCount" -> "SerialNumber=ABC,LockCount=4", the same
  // comma-separated shape PullSDK answers with. `bufferSize` is accepted and
  // ignored -- there is no fixed buffer here -- so the call sites need no
  // #ifdef.
  std::string GetDeviceParam(const std::string& items, int bufferSize, int* retOut = nullptr);

  // Not implemented: the C3 command set ported here has GETPARAM but no
  // SETPARAM. Returns a negative code and records LastError() rather than
  // pretending to have written something, because a silent no-op here would
  // look like a device that accepts settings and ignores them.
  int SetDeviceParam(const std::string& itemValues);

  // operationId matches PullSDK's table, which is the same table C3 uses:
  // 1 = output, 2 = cancel alarm, 3 = restart, 4 = enable/disable normal open.
  int ControlDevice(int operationId, int param1, int param2, int param3, int param4,
                    const std::string& options = std::string());

  // PullSDK-format CSV, "\r\n" separated. Empty when nothing is pending.
  std::string GetRTLog(int bufferSize, int* retOut = nullptr);

  std::string LastError() const { return lastError_; }

  // Which handshake the panel accepted. Useful in diagnostics: a panel that
  // only answers session-less is a real and common configuration, not a fault.
  bool IsSessionLess() const { return sessionLess_; }
  uint16_t SessionId() const { return sessionId_; }

 private:
  // Sends one frame and waits for one complete reply frame. Handles partial
  // reads: TCP gives no message boundaries, and a panel is free to split a
  // frame across segments.
  bool Transact(const c3::Bytes& request, c3::Bytes& reply, int timeoutMs);
  bool ReadOneFrame(c3::Bytes& frame, int timeoutMs);
  // The request number increments per request once a session exists; the panel
  // echoes it back and a mismatch means replies have gone out of step.
  int32_t NextRequestNr();

  TcpSocket socket_;
  std::string host_;
  int port_ = 4370;
  int timeoutMs_ = 2000;
  std::string password_;
  bool connected_ = false;
  bool sessionLess_ = false;
  uint16_t sessionId_ = 0;
  int32_t requestNr_ = 0;
  std::string lastError_;
  // Anything read from the socket that was not part of the frame just
  // consumed. A panel can coalesce two replies into one segment.
  c3::Bytes carry_;
  // Once the panel says it has no binary RT log mode, stay switched to the
  // key/value command -- matching zkaccess-c3-py's own behaviour rather than
  // re-probing on every poll.
  bool rtlogKeyValueMode_ = false;
};

}  // namespace hsf
