-- plc.lua
-- PLC communication. No REST API in this module.
--
-- Uses the ad-hoc Modbus.*(ip, port, ...) form (a one-shot connection per
-- call) rather than the gateway's own configured PLC connection, so this
-- module works standalone regardless of what config.json's modbus section
-- points at.

local Config = require("config")

local Plc = {}

------------------------------------------------------------
-- Dashboard I/O map
------------------------------------------------------------

-- Names these coils for the Dashboard's PLC Input / PLC Output panels, which
-- otherwise have nothing to show. Declaration only -- the gateway polls the
-- addresses itself; nothing here reads them. Registrations are cleared
-- whenever a script starts, so this runs on every load of this module.

-- Third argument names the address space (see Config.INPUT_SOURCE). Without
-- it the gateway defaults to discrete inputs (FC02), which on this PLC
-- answers with a constant -- the input row would sit at OFF forever.
Modbus.RegisterInput("Card Taken", Config.INPUT_CARD_TAKEN, Config.INPUT_SOURCE)
Modbus.RegisterInput("Card Source Low", Config.INPUT_CARD_SOURCE_LOW, Config.INPUT_SOURCE)
Modbus.RegisterInput("Card Reject Full", Config.INPUT_CARD_REJECT_FULL, Config.INPUT_SOURCE)
Modbus.RegisterInput("Card Not In Hopper", Config.INPUT_CARD_NOT_IN_HOPPER, Config.INPUT_SOURCE)

Modbus.RegisterOutput("Card Scan", Config.OUTPUT_SCAN)
Modbus.RegisterOutput("Card Out", Config.OUTPUT_OUT)
Modbus.RegisterOutput("Card Collect", Config.OUTPUT_COLLECT)

------------------------------------------------------------
-- Trigger PLC outputs
------------------------------------------------------------

function Plc.PulseOutput(address)
    Modbus.WriteCoil(Config.PLC_IP, Config.PLC_PORT, address, true)
    Sleep(100)
    Modbus.WriteCoil(Config.PLC_IP, Config.PLC_PORT, address, false)
end

function Plc.MoveCardToScan()
    Plc.PulseOutput(Config.OUTPUT_SCAN)
end

function Plc.MoveCardOut()
    Plc.PulseOutput(Config.OUTPUT_OUT)
end

function Plc.MoveCardIn()
    Plc.PulseOutput(Config.OUTPUT_IN)
end

function Plc.CollectCard()
    Plc.PulseOutput(Config.OUTPUT_COLLECT)
end

------------------------------------------------------------
-- Read PLC inputs / outputs
------------------------------------------------------------

-- Discrete input (FC02), not a coil (FC01). Physical input terminals live in
-- a separate read-only Modbus address space, and a coil read of the same
-- address fails or returns unrelated data on PLCs that map them that way --
-- which is why WaitCardTaken never saw the sensor.
function Plc.ReadInput(address)
    local state = Modbus.ReadDiscreteInput(Config.PLC_IP, Config.PLC_PORT, address)
    --Log.Info("ReadInput: address=" .. tostring(address) .. ", state=" .. tostring(state))
    return state
end

-- Outputs stay coils (FC01) -- that is the writable space they're driven in.
function Plc.ReadOutput(address)
    local state = Modbus.ReadCoil(Config.PLC_IP, Config.PLC_PORT, address)
    Log.Info("ReadOutput: address=" .. tostring(address) .. ", state=" .. tostring(state))
    return state
end

------------------------------------------------------------
-- Machine status inputs
------------------------------------------------------------

-- Reads the three consumable/jam sensors in one go (discrete inputs 1025-1027).
-- Pure I/O: what to do about a set flag -- block the workflow, raise a ticket
-- with the server, warn the operator -- is machine.lua's decision, not this
-- module's.
--
-- A field is nil, not false, when the read itself failed: an unreachable PLC
-- must not report as "hopper fine". Callers treat nil as unknown.
function Plc.ReadMachineStatus()

    return {
        source_low   = Plc.ReadInput(Config.INPUT_CARD_SOURCE_LOW),
        reject_full  = Plc.ReadInput(Config.INPUT_CARD_REJECT_FULL),
        hopper_empty = Plc.ReadInput(Config.INPUT_CARD_NOT_IN_HOPPER),
    }

end

------------------------------------------------------------
-- Wait for sensors
------------------------------------------------------------

function Plc.WaitCardTaken(timeoutMs)

    local elapsed = 0

    while elapsed < timeoutMs do

        if Plc.ReadInput(Config.INPUT_CARD_TAKEN) then
            return true
        end

        Sleep(1)
        elapsed = elapsed + 100

    end

    return false

end

return Plc
