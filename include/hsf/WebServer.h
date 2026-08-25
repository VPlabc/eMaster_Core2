#pragma once

#include <memory>
#include <string>

#include "hsf/ConfigManager.h"

namespace hsf {

class RestClient;
class SerialPort;
class ModbusClient;
class RfidClient;
class LuaRuntimeManager;
class MqClient;
class ZkController;
class UpdateManager;
class PackageManager;
class PluginManager;

// Hosts the monitoring/configuration web UI: serves the static frontend
// (web/), exposes the REST API the dashboard and config pages call, and
// pushes realtime system stats + runtime variables to browsers over a
// WebSocket. Implemented with Crow; all Crow types are hidden behind the
// pimpl in WebServer.cpp so this header stays cheap to include.
class WebServer {
 public:
  WebServer();
  ~WebServer();

  void Configure(const WebConfig& config, const std::string& webRoot);
  // `serial2` is the LED display port. Needed here for the same reason as
  // `serial`: the gateway holds both open for its whole run, so the
  // Configuration page's test buttons have to work THROUGH the live port
  // instead of trying to open a second exclusive handle to it.
  // `zk` is the gateway's OWN controller session, not a probe: the Test Tool
  // (Test Tool plan sections 41-48) has to drive the same connection the Lua
  // application uses, or its RTLog stream would be a second SDK session showing
  // events the running system never saw. That sharing is also why its relay
  // controls are admin-only -- they can open a real door out from under the
  // locker state machine.
  // `update` is the OTA manager. It is here rather than driven from a page of
  // its own because the update popup has to be reachable from every page --
  // an operator who is looking at the Logs tab when a security release lands
  // should see it there.
  void SetModules(RestClient* rest, SerialPort* serial, SerialPort* serial2, ModbusClient* modbus,
                   RfidClient* rfid, LuaRuntimeManager* lua, MqClient* mq, ZkController* zk,
                   UpdateManager* update);

  // Production Lua packaging (request/AdvanceUpdate.md Phase 2). Its own
  // setter rather than a tenth parameter on SetModules: that list is already
  // long enough that a caller has to count commas to see what it is passing.
  void SetPackageManager(PackageManager* packages);
  void SetPluginManager(PluginManager* plugins);

  // Starts the HTTP/WebSocket server and the realtime broadcast loop on
  // background threads; returns immediately.
  void Start();
  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hsf
