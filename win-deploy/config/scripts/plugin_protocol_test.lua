-- Select one installed protocol plugin and run its active health probe.
-- Start this file from Lua Editor after installing/enabling a plugin.
--
-- Modbus TCP: hsf.driver.modbus (uses config/plugins/<id> transport settings)
-- C3 protocol: hsf.driver.c3protocol (TCP port 4370)

local selected = "hsf.driver.modbus"
-- Change to "hsf.driver.c3protocol" to test the C3/InBio panel.

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
