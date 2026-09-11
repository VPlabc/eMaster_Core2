-- Native plugin smoke test for C3Protocol and Modbus TCP.
-- Uses only the gateway's Plugin and Config Lua interfaces.

local HEALTH_ATTEMPTS = 3
local RETRY_DELAY_MS = 500

local TESTS = {
    {
        id = "hsf.driver.c3protocol",
        label = "C3Protocol",
        result_variable = "C3NativePluginTest",
        default_port = 4370
    },
    {
        id = "hsf.driver.modbus",
        label = "Modbus TCP",
        result_variable = "ModbusNativePluginTest",
        default_port = 502
    }
}

local function info(message)
    Log.Info("[Native plugins test] " .. message)
end

local function fail(message)
    Log.Error("[Native plugins test] " .. message)
end

local installed = {}
for _, plugin in ipairs(Plugin.List()) do
    installed[plugin.id] = plugin
end

local all_config = Config.GetCategory("plugins") or {}
local plugins_passed = 0

for _, test in ipairs(TESTS) do
    local record = installed[test.id]
    local result

    if record == nil then
        result = "FAIL: plugin missing"
        fail(test.label .. " " .. result)
    elseif record.state ~= "RUNNING" then
        result = "FAIL: state=" .. tostring(record.state)
        fail(test.label .. " " .. result)
    else
        local config = all_config[test.id] or {}
        local transport = config.transport or {}
        local host = transport.host or transport.ip
        local port = tonumber(transport.port) or test.default_port
        local timeout_ms = tonumber(config.timeout_ms or transport.timeout_ms) or 2000

        if transport.type ~= "tcp" or host == nil or host == "" then
            result = "FAIL: TCP transport is not configured"
            fail(test.label .. " " .. result)
        else
            info(test.label .. " " .. tostring(record.version) ..
                 " tcp://" .. tostring(host) .. ":" .. tostring(port) ..
                 " timeout_ms=" .. tostring(timeout_ms))

            local health_passed = 0
            local last_error = nil
            for attempt = 1, HEALTH_ATTEMPTS do
                local healthy, health_error = Plugin.Health(test.id)
                if healthy then
                    health_passed = health_passed + 1
                    info(test.label .. " health " .. attempt .. " PASS")
                else
                    last_error = health_error
                    fail(test.label .. " health " .. attempt ..
                         " FAIL: " .. tostring(health_error))
                end

                if attempt < HEALTH_ATTEMPTS then
                    Sleep(RETRY_DELAY_MS)
                end
            end

            if health_passed == HEALTH_ATTEMPTS then
                result = "PASS: health=" .. health_passed .. "/" .. HEALTH_ATTEMPTS
                plugins_passed = plugins_passed + 1
                info(test.label .. " " .. result)
            else
                result = "FAIL: health=" .. health_passed .. "/" .. HEALTH_ATTEMPTS ..
                         ", error=" .. tostring(last_error)
                fail(test.label .. " " .. result)
            end
        end
    end

    SetVariable(test.result_variable, result)
end

local summary = "plugins=" .. plugins_passed .. "/" .. #TESTS
SetVariable("NativePluginsTest", summary)

if plugins_passed == #TESTS then
    info("PASS: " .. summary)
else
    fail("FAIL: " .. summary)
end

-- The current Plugin Lua table exposes List() and Health() only. Native C3
-- RTLog/relay/input operations and Modbus FC reads/writes are covered by each
-- plugin's C mock-transport test until generic Plugin.Read/Write/Command Lua
-- bindings are added to the gateway.
