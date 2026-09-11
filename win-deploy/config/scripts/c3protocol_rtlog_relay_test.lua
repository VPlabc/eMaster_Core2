-- C3Protocol RTLog and output pulse test.
--
-- The native plugin is used for discovery, configuration, and its active
-- health probe. RTLog and relay operations then use zk_controller because the
-- current Plugin Lua API exposes List/Health only (no Plugin.Command yet).

local zk = require("zk_controller")
local PLUGIN_ID = "hsf.driver.c3protocol"
local MONITOR_MS = 30000
local POLL_MS = 200

-- CAUTION: running this test pulses all four outputs. Normal relay outputs may
-- unlock doors. Set false when testing RTLog only.
local ENABLE_OUTPUT_PULSES = true
local PULSE_MS = 500
local BETWEEN_PULSES_MS = 500

local OUTPUTS = {
    { name = "relay 1", number = 1, auxiliary = false },
    { name = "relay 2", number = 2, auxiliary = false },
    { name = "AUX 1", number = 1, auxiliary = true },
    { name = "AUX 2", number = 2, auxiliary = true }
}

local raw_count = 0
local card_count = 0
local pulse_passed = 0

local function info(message)
    Log.Info("[C3 test] " .. message)
end

local function fail(message)
    Log.Error("[C3 test] " .. message)
end

local plugin_record = nil
for _, plugin in ipairs(Plugin.List()) do
    if plugin.id == PLUGIN_ID then
        plugin_record = plugin
        break
    end
end

if plugin_record == nil then
    fail("plugin is not installed: " .. PLUGIN_ID)
    SetVariable("C3ProtocolTest", "FAIL: plugin missing")
    return
end

if plugin_record.state ~= "RUNNING" then
    fail("plugin is not enabled; current state=" .. tostring(plugin_record.state))
    SetVariable("C3ProtocolTest", "FAIL: plugin not running")
    return
end

local plugins_config = Config.GetCategory("plugins") or {}
local plugin_config = plugins_config[PLUGIN_ID] or {}
local transport = plugin_config.transport or {}
local device_ip = transport.host or transport.ip
local device_port = tonumber(transport.port) or 4370
local timeout_ms = tonumber(plugin_config.timeout_ms or transport.timeout_ms) or 2000

if device_ip == nil or device_ip == "" then
    fail("plugin TCP host is not configured")
    SetVariable("C3ProtocolTest", "FAIL: plugin config")
    return
end

local healthy, health_error = Plugin.Health(PLUGIN_ID)
if not healthy then
    fail("plugin health failed: " .. tostring(health_error))
    SetVariable("C3ProtocolTest", "FAIL: plugin health")
    return
end
info("plugin health PASS for " .. device_ip .. ":" .. device_port)

zk.onConnectionChanged(function(connected)
    info("connection changed: " .. (connected and "connected" or "disconnected"))
end)

-- Receives every non-empty GetRTLog response, including card, door, alarm,
-- and auxiliary-input records.
zk.onRawRTLog(function(raw)
    raw_count = raw_count + 1
    info("RTLog #" .. raw_count .. ": " .. tostring(raw))
end)

-- Parsed card records are also shown in a more convenient form.
zk.onCard(function(event)
    card_count = card_count + 1
    info("card #" .. card_count ..
         " data=" .. tostring(event.CardData) ..
         " door=" .. tostring(event.DoorID) ..
         " reader=" .. tostring(event.ReaderID) ..
         " event=" .. tostring(event.EventType) ..
         " verify=" .. tostring(event.VerifyMode))
end)

info("connecting RTLog/control client to " .. device_ip .. ":" .. device_port)
zk.setAutoReconnect(true)
local connected, connect_error = zk.connect(device_ip, device_port, timeout_ms, "")
if not connected then
    fail("RTLog/control connection failed: " .. tostring(connect_error))
    SetVariable("C3ProtocolTest", "FAIL: connect")
    return
end

zk.startRTLog()
info("RTLog monitoring started for " .. MONITOR_MS .. " ms")

if ENABLE_OUTPUT_PULSES then
    for _, output in ipairs(OUTPUTS) do
        info("pulsing " .. output.name .. " for " .. PULSE_MS .. " ms")
        local ok, err = zk.pulse(output.number, PULSE_MS, output.auxiliary)
        if ok then
            pulse_passed = pulse_passed + 1
            info(output.name .. " pulse PASS")
        else
            fail(output.name .. " pulse FAIL: " .. tostring(err or zk.lastError()))
        end
        Sleep(BETWEEN_PULSES_MS)
    end
else
    info("relay pulses skipped; set ENABLE_OUTPUT_PULSES=true to test relay 1, relay 2, AUX 1, and AUX 2")
end

local elapsed = 0
while elapsed < MONITOR_MS do
    Sleep(POLL_MS)
    elapsed = elapsed + POLL_MS
end

-- RTLog is intentionally left running because the controller is shared with
-- the gateway and other Lua packages may depend on it.
local result = "RTLog=" .. raw_count ..
               ", cards=" .. card_count ..
               ", pulses=" .. pulse_passed .. "/" .. #OUTPUTS
SetVariable("C3ProtocolTest", result)
info("complete: " .. result)
