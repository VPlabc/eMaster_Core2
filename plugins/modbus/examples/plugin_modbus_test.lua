-- Modbus plugin smoke test.
--
-- Enable hsf.driver.modbus from the Plugins page before running this script.
-- This test does not read or write a PLC register; it verifies that the
-- native plugin is installed, configured, loaded, and passing its health
-- probe.

local plugin_id = "hsf.driver.modbus"
local installed = false
local state = "missing"

for _, plugin in ipairs(Plugin.List()) do
    if plugin.id == plugin_id then
        installed = true
        state = plugin.state or "unknown"
        break
    end
end

local config = Config.GetCategory("plugins")
local plugin_config = config and config[plugin_id]
local configured = plugin_config ~= nil and plugin_config.transport ~= nil

if not installed then
    local message = "Modbus plugin is not installed: " .. plugin_id
    Log.Error(message)
    SetVariable("ModbusPluginTest", "FAIL: " .. message)
elseif not configured then
    local message = "Modbus plugin has no transport configuration"
    Log.Error(message)
    SetVariable("ModbusPluginTest", "FAIL: " .. message)
else
    local healthy, error_message = Plugin.Health(plugin_id)
    if healthy then
        local message = "PASS: " .. plugin_id .. " state=" .. state
        Log.Info(message)
        SetVariable("ModbusPluginTest", message)
    else
        local message = "Modbus plugin health failed: " .. tostring(error_message)
        Log.Error(message)
        SetVariable("ModbusPluginTest", "FAIL: " .. message)
    end
end
