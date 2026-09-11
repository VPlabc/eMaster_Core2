-- Modbus protocol examples using the gateway's configured PLC connection.
-- Addresses passed to the Lua API are zero-based protocol addresses.
-- Edit these values to match registers that exist on your PLC.

local ADDRESS = {
    coil = 0,              -- FC01 / FC05
    discrete_input = 0,    -- FC02
    holding_register = 0,  -- FC03 / FC06
    input_register = 0     -- FC04
}

-- Keep writes off until the addresses and test values are safe for the PLC.
local ENABLE_WRITES = false
local COIL_TEST_VALUE = true
local HOLDING_TEST_VALUE = 123

local function show(label, value)
    if value == nil then
        Log.Error(label .. " failed (no reply or Modbus exception)")
        return false
    end
    Log.Info(label .. " = " .. tostring(value))
    return true
end

if not Modbus.IsConnected() then
    Log.Error("Modbus is not connected; check Configuration > Modbus")
    SetVariable("ModbusProtocolExample", "FAIL: not connected")
    return
end

local passed = 0

-- FC01: Read Coils
if show("FC01 coil " .. ADDRESS.coil, Modbus.ReadCoil(ADDRESS.coil)) then
    passed = passed + 1
end

-- FC02: Read Discrete Inputs
if show("FC02 discrete input " .. ADDRESS.discrete_input,
        Modbus.ReadDiscreteInput(ADDRESS.discrete_input)) then
    passed = passed + 1
end

-- FC03: Read Holding Registers
if show("FC03 holding register " .. ADDRESS.holding_register,
        Modbus.ReadHolding(ADDRESS.holding_register)) then
    passed = passed + 1
end

-- FC04: Read Input Registers
if show("FC04 input register " .. ADDRESS.input_register,
        Modbus.ReadInputRegister(ADDRESS.input_register)) then
    passed = passed + 1
end

if ENABLE_WRITES then
    -- FC05: Write Single Coil
    local coil_ok = Modbus.WriteCoil(ADDRESS.coil, COIL_TEST_VALUE)
    show("FC05 write coil " .. ADDRESS.coil, coil_ok and COIL_TEST_VALUE or nil)

    -- FC06: Write Single Holding Register
    local register_ok = Modbus.WriteHolding(ADDRESS.holding_register, HOLDING_TEST_VALUE)
    show("FC06 write holding register " .. ADDRESS.holding_register,
         register_ok and HOLDING_TEST_VALUE or nil)
else
    Log.Info("FC05/FC06 writes skipped; set ENABLE_WRITES=true after checking addresses")
end

local result = "Read tests passed: " .. tostring(passed) .. "/4"
SetVariable("ModbusProtocolExample", result)
Log.Info(result)
