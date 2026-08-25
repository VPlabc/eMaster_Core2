-- Select an installed protocol plugin and run its active health probe.
-- Enable the selected plugin in the Plugins page before running this script.

local selected = "hsf.driver.c3protocol"
-- Use "hsf.driver.modbus" to test Modbus TCP instead.

local found = false
for _, plugin in ipairs(Plugin.List()) do
    if plugin.id == selected then
        found = true
        Log.Info("Testing protocol plugin " .. plugin.id .. " state=" .. plugin.state)
        local ok, err = Plugin.Health(plugin.id)
        if ok then
            Log.Info(plugin.id .. " health test passed")
        else
            Log.Error(plugin.id .. " health test failed: " .. tostring(err))
        end
    end
end

if not found then
    Log.Error("Protocol plugin is not installed: " .. selected)
end
