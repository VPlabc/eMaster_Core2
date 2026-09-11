#pragma once

#include <memory>
#include <string>

#include "hsf/ConfigManager.h"
#include "hsf/ServiceRegistry.h"

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
  // Services are resolved by name/capability from the registry instead of
  // being passed as a fixed positional list. This keeps current behaviour
  // while removing the chokepoint that made every new built-in module a
  // header/signature change here and in main.cpp.
  void SetServices(ServiceRegistry* services);

  // Starts the HTTP/WebSocket server and the realtime broadcast loop on
  // background threads; returns immediately.
  void Start();
  void Stop();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hsf
