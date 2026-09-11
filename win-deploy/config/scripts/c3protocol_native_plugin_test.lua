-- Strict native hsf.driver.c3protocol plugin test.
-- This script intentionally does not require or call zk_controller.

local PLUGIN_ID = "hsf.driver.c3protocol"
local HEALTH_ATTEMPTS = 3
local BETWEEN_ATTEMPTS_MS = 500

local function info(message)
    Log.Info("[C3 native plugin] " .. message)
end

local function fail(message)
    Log.Error("[C3 native plugin] " .. message)
end

local plugin_record = nil
for _, plugin in ipairs(Plugin.List()) do
    if plugin.id == PLUGIN_ID then
        plugin_record = plugin
        break
    end
end

if plugin_record == nil then
    fail("FAIL: plugin is not installed")
    SetVariable("C3NativePluginTest", "FAIL: plugin missing")
    return
end

info("found " .. plugin_record.id ..
     " version=" .. tostring(plugin_record.version) ..
     " state=" .. tostring(plugin_record.state) ..
     " enabled=" .. tostring(plugin_record.enabled))

if plugin_record.state ~= "RUNNING" then
    fail("FAIL: plugin must be enabled and RUNNING")
    SetVariable("C3NativePluginTest", "FAIL: state=" .. tostring(plugin_record.state))
    return
end

local plugins_config = Config.GetCategory("plugins") or {}
local plugin_config = plugins_config[PLUGIN_ID] or {}
local transport = plugin_config.transport or {}
local host = transport.host or transport.ip
local port = tonumber(transport.port)
local transport_timeout_ms = tonumber(transport.timeout_ms)
local driver_timeout_ms = tonumber(plugin_config.timeout_ms)

if transport.type ~= "tcp" or host == nil or port == nil then
    fail("FAIL: native plugin TCP transport configuration is incomplete")
    SetVariable("C3NativePluginTest", "FAIL: configuration")
    return
end

info("config tcp://" .. tostring(host) .. ":" .. tostring(port) ..
     " transport_timeout_ms=" .. tostring(transport_timeout_ms) ..
     " driver_timeout_ms=" .. tostring(driver_timeout_ms))

local passed = 0
for attempt = 1, HEALTH_ATTEMPTS do
    -- Plugin.Health invokes the native driver's health() vtable method. For
    -- c3protocol this sends a session-less C3 connect frame and validates the
    -- protocol reply through the plugin-owned TCP transport.
    local healthy, health_error = Plugin.Health(PLUGIN_ID)
    if healthy then
        passed = passed + 1
        info("native health attempt " .. attempt .. " PASS")
    else
        fail("native health attempt " .. attempt ..
             " FAIL: " .. tostring(health_error))
    end

    if attempt < HEALTH_ATTEMPTS then
        Sleep(BETWEEN_ATTEMPTS_MS)
    end
end

local result = "health=" .. passed .. "/" .. HEALTH_ATTEMPTS
SetVariable("C3NativePluginTest", result)

if passed == HEALTH_ATTEMPTS then
    info("PASS: " .. result)
else
    fail("FAIL: " .. result)
end

-- The current native c3protocol driver exposes only connection status through
-- read(), rejects write() with HSF_ERR_NOT_SUPPORTED, and has no command()
-- implementation. Consequently native RTLog and relay pulse tests cannot be
-- performed until those driver operations and Lua bindings are implemented.
