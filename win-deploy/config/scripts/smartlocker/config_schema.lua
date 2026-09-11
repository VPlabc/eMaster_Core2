-- Dynamic configuration schema for the SmartLocker Edge Logic project.
return {
    version = 1,
    title = "SmartLocker Configuration",
    groups = {
        {
            key = "server",
            label = "Server",
            fields = {
                { key = "base_url", label = "Server URL", type = "url", default = "", required = true },
                { key = "api_key", label = "API Key", type = "password" },
            },
        },
        {
            key = "plc",
            label = "PLC",
            fields = {
                { key = "host", label = "Host", type = "string", default = "192.168.1.12", required = true },
                { key = "port", label = "Port", type = "number", default = 502, min = 1, max = 65535, step = 1 },
                { key = "address_mode", label = "Address Mode", type = "select", default = "locker_io", options = {
                    { value = "locker_io", label = "Locker I/O" },
                    { value = "modicon", label = "Modicon" },
                    { value = "raw", label = "Raw" },
                } },
                { key = "read_cache", label = "Read Cache", type = "boolean", default = true },
            },
        },
        {
            key = "locker",
            label = "Locker Behavior",
            fields = {
                { key = "lock_trigger_ms", label = "Lock Trigger", type = "number", default = 100, min = 0, max = 10000, unit = "ms" },
                { key = "door_open_timeout", label = "Door Open Timeout", type = "number", default = 30, min = 1, max = 3600, unit = "s" },
                { key = "assign_on_scan", label = "Assign on Scan", type = "boolean", default = true },
                { key = "allow_cross_type", label = "Allow Cross Type", type = "boolean", default = false },
            },
        },
    },
}
