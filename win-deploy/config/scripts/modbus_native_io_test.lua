-- Native hsf.driver.modbus Modbus TCP integration test.
--
-- Uses only Plugin.Read/Plugin.Write and the native plugin ABI. Addresses are
-- zero-based Modbus protocol addresses. Edit them for the target PLC.

local PLUGIN_ID = "hsf.driver.modbus"

local ADDRESS = {
    coil = 0,               -- FC01 / FC05
    discrete_input = 0,     -- FC02
    holding_register = 0,   -- FC03 / FC06
    input_register = 0,     -- FC04
    coil_block = 0,         -- FC01 / FC15
    holding_block = 0       -- FC03 / FC16
}

local COIL_BLOCK_COUNT = 9
local HOLDING_BLOCK_COUNT = 2

-- CAUTION: running this script writes to the PLC. Every write is read back and
-- the original value is restored, but the temporary values may activate real
-- equipment. Confirm all addresses before starting the script.
local ENABLE_WRITES = true

local passed = 0
local failed = 0

local function info(message)
    Log.Info("[Modbus native I/O] " .. message)
end

local function fail(message)
    Log.Error("[Modbus native I/O] " .. message)
end

local function display_value(value)
    if type(value) ~= "string" then
        return tostring(value)
    end
    return "0x" .. (value:gsub(".", function(byte)
        return string.format("%02X", string.byte(byte))
    end))
end

local function read_value(function_code, address, count, label)
    local value, err = Plugin.Read(PLUGIN_ID, function_code, address, count or 1)
    if value == nil then
        fail(label .. " read failed: " .. tostring(err))
        return nil
    end
    info(label .. " read PASS: " .. display_value(value))
    return value
end

local function write_value(function_code, address, value, count, label)
    local ok, err = Plugin.Write(
        PLUGIN_ID, function_code, address, value, count or 1)
    if not ok then
        fail(label .. " write failed: " .. tostring(err))
        return false
    end
    info(label .. " write PASS")
    return true
end

local function byte_bit(byte, bit)
    return math.floor(byte / (2 ^ bit)) % 2
end

local function bit_at(data, index)
    local byte_index = math.floor(index / 8) + 1
    local bit_index = index % 8
    return byte_bit(string.byte(data, byte_index), bit_index)
end

local function invert_bits(data, count)
    local bytes = {}
    local byte_count = math.floor((count + 7) / 8)
    for byte_index = 1, byte_count do
        local value = 0
        for bit = 0, 7 do
            local index = (byte_index - 1) * 8 + bit
            if index < count and bit_at(data, index) == 0 then
                value = value + (2 ^ bit)
            end
        end
        bytes[byte_index] = string.char(value)
    end
    return table.concat(bytes)
end

local function bits_equal(left, right, count)
    if #left < math.floor((count + 7) / 8) or
       #right < math.floor((count + 7) / 8) then
        return false
    end
    for index = 0, count - 1 do
        if bit_at(left, index) ~= bit_at(right, index) then
            return false
        end
    end
    return true
end

local function change_registers(data, count)
    if #data ~= count * 2 then
        return nil
    end
    local bytes = {}
    for register = 0, count - 1 do
        local offset = register * 2 + 1
        local high, low = string.byte(data, offset, offset + 1)
        local changed = (high * 256 + low + 1) % 65536
        bytes[#bytes + 1] = string.char(math.floor(changed / 256), changed % 256)
    end
    return table.concat(bytes)
end

local function record_read(function_code, address, label)
    local value = read_value(function_code, address, 1, label)
    if value == nil then
        failed = failed + 1
    else
        passed = passed + 1
    end
    return value
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
    SetVariable("ModbusNativePluginTest", "FAIL: plugin missing")
    return
end

info("found version=" .. tostring(plugin_record.version) ..
     " state=" .. tostring(plugin_record.state) ..
     " enabled=" .. tostring(plugin_record.enabled))

if plugin_record.state ~= "RUNNING" then
    fail("plugin must be enabled and RUNNING")
    SetVariable("ModbusNativePluginTest",
                "FAIL: state=" .. tostring(plugin_record.state))
    return
end

local healthy, health_error = Plugin.Health(PLUGIN_ID)
if not healthy then
    fail("native health failed: " .. tostring(health_error))
    SetVariable("ModbusNativePluginTest",
                "FAIL: health=" .. tostring(health_error))
    return
end
info("native health PASS")

-- FC01, FC02, FC03, FC04 single-point reads.
local original_coil = record_read(1, ADDRESS.coil, "FC01 coil")
record_read(2, ADDRESS.discrete_input, "FC02 discrete input")
local original_holding = record_read(
    3, ADDRESS.holding_register, "FC03 holding register")
record_read(4, ADDRESS.input_register, "FC04 input register")

if ENABLE_WRITES then
    -- FC05: write one coil, verify with FC01, then restore it.
    if original_coil ~= nil then
        local test_value = not original_coil
        local wrote = write_value(5, ADDRESS.coil, test_value, 1, "FC05 coil")
        local verified = wrote and
            read_value(1, ADDRESS.coil, 1, "FC05 verify") == test_value
        local restored = write_value(
            5, ADDRESS.coil, original_coil, 1, "FC05 restore")
        if wrote and verified and restored then
            passed = passed + 1
        else
            failed = failed + 1
            fail("FC05 test or restoration failed")
        end
    else
        failed = failed + 1
        fail("FC05 skipped because the original coil could not be read")
    end

    -- FC06: write one holding register, verify with FC03, then restore it.
    if original_holding ~= nil then
        local test_value = (original_holding + 1) % 65536
        local wrote = write_value(
            6, ADDRESS.holding_register, test_value, 1, "FC06 register")
        local verified = wrote and read_value(
            3, ADDRESS.holding_register, 1, "FC06 verify") == test_value
        local restored = write_value(
            6, ADDRESS.holding_register, original_holding, 1, "FC06 restore")
        if wrote and verified and restored then
            passed = passed + 1
        else
            failed = failed + 1
            fail("FC06 test or restoration failed")
        end
    else
        failed = failed + 1
        fail("FC06 skipped because the original register could not be read")
    end

    -- FC15: invert a block of packed coil bits, verify, then restore the exact
    -- packed bytes returned by FC01.
    local original_coils = read_value(
        1, ADDRESS.coil_block, COIL_BLOCK_COUNT, "FC15 original coils")
    if type(original_coils) == "string" then
        local test_coils = invert_bits(original_coils, COIL_BLOCK_COUNT)
        local wrote = write_value(
            15, ADDRESS.coil_block, test_coils, COIL_BLOCK_COUNT, "FC15 coils")
        local actual = wrote and read_value(
            1, ADDRESS.coil_block, COIL_BLOCK_COUNT, "FC15 verify") or nil
        local verified = type(actual) == "string" and
                         bits_equal(actual, test_coils, COIL_BLOCK_COUNT)
        local restored = write_value(
            15, ADDRESS.coil_block, original_coils, COIL_BLOCK_COUNT,
            "FC15 restore")
        if wrote and verified and restored then
            passed = passed + 1
        else
            failed = failed + 1
            fail("FC15 test or restoration failed")
        end
    else
        failed = failed + 1
        fail("FC15 skipped because the original coil block could not be read")
    end

    -- FC16: increment each big-endian register by one, verify with FC03, then
    -- restore the original register bytes.
    local original_registers = read_value(
        3, ADDRESS.holding_block, HOLDING_BLOCK_COUNT,
        "FC16 original registers")
    local test_registers = type(original_registers) == "string" and
        change_registers(original_registers, HOLDING_BLOCK_COUNT) or nil
    if test_registers ~= nil then
        local wrote = write_value(
            16, ADDRESS.holding_block, test_registers, HOLDING_BLOCK_COUNT,
            "FC16 registers")
        local actual = wrote and read_value(
            3, ADDRESS.holding_block, HOLDING_BLOCK_COUNT, "FC16 verify") or nil
        local verified = actual == test_registers
        local restored = write_value(
            16, ADDRESS.holding_block, original_registers,
            HOLDING_BLOCK_COUNT, "FC16 restore")
        if wrote and verified and restored then
            passed = passed + 1
        else
            failed = failed + 1
            fail("FC16 test or restoration failed")
        end
    else
        failed = failed + 1
        fail("FC16 skipped because the original register block could not be read")
    end
else
    info("FC05/FC06/FC15/FC16 skipped because ENABLE_WRITES=false")
end

local expected = ENABLE_WRITES and 8 or 4
local result = string.format("passed=%d/%d, failed=%d", passed, expected, failed)
SetVariable("ModbusNativePluginTest", result)
if passed == expected and failed == 0 then
    info("PASS: " .. result)
else
    fail("FAIL: " .. result)
end
