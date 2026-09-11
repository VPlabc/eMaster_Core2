-- C3 protocol card UID and relay output test.
--
-- The C3 native plugin supplies discovery/configuration and health checking.
-- The zk_controller client uses the same C3 transport for RTLog/card events
-- and relay commands (the native Plugin Lua API does not expose commands).

local PLUGIN_ID = "hsf.driver.c3protocol"
local WAIT_MS = 60000
local POLL_MS = 100
local PULSE_MS = 500
local BETWEEN_PULSES_MS = 500

-- CAUTION: physical relay outputs may unlock doors.
local ENABLE_OUTPUT_PULSES = true

local OUTPUTS = {
    { name = "Relay 1", number = 1, auxiliary = false },
    { name = "Relay 2", number = 2, auxiliary = false },
    { name = "AUX1", number = 1, auxiliary = true },
    { name = "AUX2", number = 2, auxiliary = true }
}

local function info(message)
    Log.Info("[C3 card test] " .. message)
end

local function fail(message)
    Log.Error("[C3 card test] " .. message)
end

local plugin_record
for _, plugin in ipairs(Plugin.List()) do
    if plugin.id == PLUGIN_ID then
        plugin_record = plugin
        break
    end
end

if plugin_record == nil then
    fail("plugin is not installed: " .. PLUGIN_ID)
    SetVariable("C3ProtocolCardRelayTest", "FAIL: plugin missing")
    return
end

if plugin_record.state ~= "RUNNING" then
    fail("plugin is not running; state=" .. tostring(plugin_record.state))
    SetVariable("C3ProtocolCardRelayTest", "FAIL: plugin not running")
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
    SetVariable("C3ProtocolCardRelayTest", "FAIL: plugin config")
    return
end

local zk = require("zk_controller")
local healthy, health_error = Plugin.Health(PLUGIN_ID)
if not healthy then
    fail("plugin health failed: " .. tostring(health_error))
    SetVariable("C3ProtocolCardRelayTest", "FAIL: plugin health")
    return
end

local card_uid = nil
local card_count = 0
local pulse_count = 0

zk.onConnectionChanged(function(connected)
    info("connection " .. (connected and "connected" or "disconnected"))
end)

zk.onCard(function(event)
    card_count = card_count + 1
    card_uid = tostring(event.CardData or event.cardData or event.UID or event.uid or "")
    info("card UID=" .. card_uid ..
         " door=" .. tostring(event.DoorID) ..
         " reader=" .. tostring(event.ReaderID) ..
         " event=" .. tostring(event.EventType))
end)

local connected, connect_error = zk.connect(device_ip, device_port, timeout_ms, "")
if not connected then
    fail("connection failed: " .. tostring(connect_error))
    SetVariable("C3ProtocolCardRelayTest", "FAIL: connect")
    return
end

zk.startRTLog()
info("waiting up to " .. WAIT_MS .. " ms for a card UID on " .. device_ip)

local elapsed = 0
while elapsed < WAIT_MS and card_uid == nil do
    Sleep(POLL_MS)
    elapsed = elapsed + POLL_MS
end

if card_uid == nil then
    fail("no card UID received before timeout")
    SetVariable("C3ProtocolCardRelayTest", "FAIL: card timeout")
    return
end

SetVariable("C3LastCardUID", card_uid)

if ENABLE_OUTPUT_PULSES then
    for _, output in ipairs(OUTPUTS) do
        info("pulsing " .. output.name .. " for " .. PULSE_MS .. " ms")
        local ok, err = zk.pulse(output.number, PULSE_MS, output.auxiliary)
        if ok then
            pulse_count = pulse_count + 1
            info(output.name .. " pulse PASS")
        else
            fail(output.name .. " pulse FAIL: " .. tostring(err or zk.lastError()))
        end
        Sleep(BETWEEN_PULSES_MS)
    end
else
    info("output pulses disabled")
end

local result = "UID=" .. card_uid .. ", cards=" .. card_count ..
               ", pulses=" .. pulse_count .. "/" .. #OUTPUTS
SetVariable("C3ProtocolCardRelayTest", result)
info("complete: " .. result)
