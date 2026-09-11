-- Dynamic Config save/load example. See the source copy for usage details.
local schema = {
    version = 1,
    title = "Dynamic Config Test",
    groups = {
        { key = "demo", label = "Demo Settings", fields = {
            { key = "sample_parameter", label = "Sample Parameter", type = "string", default = "first-run" },
            { key = "enabled", label = "Enabled", type = "boolean", default = true },
            { key = "retry_count", label = "Retry Count", type = "number", default = 3, min = 0, max = 10, step = 1 },
            { key = "items", label = "Repeatable Items", type = "array_group", item_schema = {
                fields = {
                    { key = "name", label = "Name", type = "string", required = true },
                    { key = "value", label = "Value", type = "number", default = 1, min = 0 },
                }
            } },
        } },
    },
}

local ok, err = config.register_schema(schema)
if not ok then print("Dynamic Config Test: schema registration failed: " .. tostring(err)); return end
local loaded = config.get("sample_parameter")
print("Dynamic Config Test: loaded sample_parameter = " .. tostring(loaded))
if loaded == nil then
    local saved, save_error = config.set("sample_parameter", "saved-by-lua")
    if not saved then print("Dynamic Config Test: save failed: " .. tostring(save_error)); return end
    print("Dynamic Config Test: saved and reloaded sample_parameter = " .. tostring(config.get("sample_parameter")))
end
print("Dynamic Config Test: get_all().sample_parameter = " .. tostring(config.get_all().sample_parameter))
print("Dynamic Config Test: change this value in Dynamic Configuration, restart the script, and check this log.")
while true do Sleep(1000) end
