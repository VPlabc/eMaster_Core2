-- Dynamic configuration schema for the CardDispenser Edge Logic project.
-- The generic Config Manager loads this table; runtime values remain separate.
return {
    version = 1,
    title = "Card Dispenser Configuration",
    groups = {
        {
            key = "server",
            label = "Registration Server",
            fields = {
                { key = "server_url", label = "Server URL", type = "url", default = "http://localhost:8091", required = true },
                { key = "api_key", label = "API Key", type = "password", default = "", required = true },
            },
        },
        {
            key = "reader",
            label = "ZK Reader",
            fields = {
                { key = "zk_ip", label = "IP Address", type = "string", default = "192.168.1.201", required = true },
                { key = "zk_port", label = "Port", type = "number", default = 4370, min = 1, max = 65535, step = 1 },
                { key = "zk_timeout", label = "Timeout", type = "number", default = 2000, min = 100, max = 30000, unit = "ms" },
            },
        },
        {
            key = "timing",
            label = "Timing",
            fields = {
                { key = "card_wait_timeout", label = "Card Wait Timeout", type = "number", default = 5000, min = 100, max = 120000, unit = "ms" },
                { key = "take_card_timeout", label = "Take Card Timeout", type = "number", default = 30000, min = 100, max = 300000, unit = "ms" },
                { key = "max_scan_retry", label = "Maximum Scan Retries", type = "number", default = 3, min = 0, max = 20, step = 1 },
                { key = "enabled", label = "Enabled", type = "boolean", default = true },
            },
        },
    },
}
