-- Native hsf.driver.c3protocol integration test.
--
-- This script uses only the plugin ABI exposed through Plugin.Read/Write. It
-- does not load or call zk_controller.

local PLUGIN_ID = "hsf.driver.c3protocol"
local PULSE_MS = 500
local POLL_MS = 250
local MONITOR_MS = 30000

-- CAUTION: relay 1 and relay 2 commonly unlock doors. Running this script
-- physically pulses relay 1, relay 2, AUX 1, and AUX 2.
local ENABLE_OUTPUT_PULSES = true

local outputs = {
    { name = "relay 1", address_type = 3, number = 1 },
    { name = "relay 2", address_type = 3, number = 2 },
    { name = "AUX 1",   address_type = 4, number = 1 },
    { name = "AUX 2",   address_type = 4, number = 2 }
}

local rtlog_records = 0
local read_errors = 0
local pulse_passed = 0
local input_state = {}

local function info(message)
    Log.Info("[C3 native I/O] " .. message)
end

local function fail(message)
    Log.Error("[C3 native I/O] " .. message)
end

local function u32le(data, offset)
    local b1, b2, b3, b4 = string.byte(data, offset, offset + 3)
    return b1 + b2 * 256 + b3 * 65536 + b4 * 16777216
end

-- C3 binary RTLog time uses mixed-radix fields packed into one little-endian
-- 32-bit integer: year/month/day/hour/minute/second.
local function decode_c3_time(value)
    local second = value % 60
    value = math.floor(value / 60)
    local minute = value % 60
    value = math.floor(value / 60)
    local hour = value % 24
    value = math.floor(value / 24)
    local day = (value % 31) + 1
    value = math.floor(value / 31)
    local month = (value % 12) + 1
    local year = math.floor(value / 12) + 2000
    return string.format("%04d-%02d-%02d %02d:%02d:%02d",
                         year, month, day, hour, minute, second)
end

local function decode_rtlog(data)
    if #data == 0 then
        return 0
    end
    if (#data % 16) ~= 0 then
        fail("invalid RTLog payload length=" .. #data)
        return 0
    end

    local count = 0
    for offset = 1, #data, 16 do
        local card = u32le(data, offset)
        local pin = u32le(data, offset + 4)
        local verify = string.byte(data, offset + 8)
        local door_or_input = string.byte(data, offset + 9)
        local event_type = string.byte(data, offset + 10)
        local direction = string.byte(data, offset + 11)
        local timestamp = decode_c3_time(u32le(data, offset + 12))

        if event_type == 220 then
            input_state[door_or_input] = false
        elseif event_type == 221 then
            input_state[door_or_input] = true
        end

        count = count + 1
        rtlog_records = rtlog_records + 1
        info(string.format(
            "RTLog #%d card=%u pin=%u verify=%d door/input=%d event=%d direction=%d time=%s",
            rtlog_records, card, pin, verify, door_or_input, event_type,
            direction, timestamp))
    end
    return count
end

local function read_rtlog()
    -- Native C3 address a1=1 returns zero or more packed 16-byte RTLog records.
    local data, err = Plugin.Read(PLUGIN_ID, 1, 0)
    if data == nil then
        read_errors = read_errors + 1
        fail("RTLog read failed: " .. tostring(err))
        return false
    end
    decode_rtlog(data)
    return true
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
    SetVariable("C3NativePluginTest", "FAIL: plugin missing")
    return
end

info("found version=" .. tostring(plugin_record.version) ..
     " state=" .. tostring(plugin_record.state) ..
     " enabled=" .. tostring(plugin_record.enabled))

if plugin_record.state ~= "RUNNING" then
    fail("plugin must be enabled and RUNNING")
    SetVariable("C3NativePluginTest", "FAIL: state=" .. tostring(plugin_record.state))
    return
end

local healthy, health_error = Plugin.Health(PLUGIN_ID)
if not healthy then
    fail("native health failed: " .. tostring(health_error))
    SetVariable("C3NativePluginTest", "FAIL: health=" .. tostring(health_error))
    return
end
info("native health PASS")

-- Read once before changing outputs so communication/RTLog failures stop the
-- test before any relay is activated.
if not read_rtlog() then
    SetVariable("C3NativePluginTest", "FAIL: initial RTLog read")
    return
end

if ENABLE_OUTPUT_PULSES then
    for _, output in ipairs(outputs) do
        info("pulsing " .. output.name .. " for " .. PULSE_MS .. " ms")
        -- Native C3 write address: a1=3 for door relay, a1=4 for AUX;
        -- a2 is the output number and the integer value is duration in ms.
        local ok, err = Plugin.Write(
            PLUGIN_ID, output.address_type, output.number, PULSE_MS)
        if ok then
            pulse_passed = pulse_passed + 1
            info(output.name .. " pulse PASS")
        else
            fail(output.name .. " pulse FAIL: " .. tostring(err))
        end
        read_rtlog()
        Sleep(POLL_MS)
    end
else
    info("output pulses disabled")
end

info("monitoring native RTLog for " .. MONITOR_MS .. " ms")
local elapsed = 0
while elapsed < MONITOR_MS do
    read_rtlog()
    Sleep(POLL_MS)
    elapsed = elapsed + POLL_MS
end

for input = 1, 2 do
    if input_state[input] == nil then
        info("input " .. input .. " state not observed in RTLog")
    else
        info("input " .. input .. "=" .. (input_state[input] and "ON" or "OFF"))
    end
end

local result = string.format("RTLog=%d, pulses=%d/%d, read_errors=%d",
                             rtlog_records, pulse_passed, #outputs, read_errors)
SetVariable("C3NativePluginTest", result)
if pulse_passed == #outputs and read_errors == 0 then
    info("PASS: " .. result)
else
    fail("FAIL: " .. result)
end
