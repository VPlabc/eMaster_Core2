-- tests/test_modbus_output.lua
-- Stage 1.2 (Plan section 19): pulse every lock relay.
--
-- THIS TEST OPENS DOORS. Each address below is the unlock output of one locker,
-- so every pulse physically releases a latch. Run it with the cabinet empty.
--
-- Two checks matter. Each pulse must return to 0 -- a latch left energised is a
-- door that never locks again. And no OTHER locker's output may move while it
-- happens: that is Plan rule 10, and with one coil per lock it is also how a
-- duplicated address in the layout shows up, since two lockers mapped to the
-- same coil open in pairs.

local ok = pcall(require, "bootstrap")
if not ok then require("smartlocker.tests.bootstrap") end

local Config  = require("config")
local Harness = require("tests.harness")
local Modbus  = require("hardware.modbus")

Harness.start("Stage 1.2 -- lock relay outputs")

local lockers = {}

for _, locker in ipairs(Config.Lockers()) do
    if locker.output_register ~= nil then
        lockers[#lockers + 1] = locker
    end
end

local holdMs = Config.locker.lock_trigger_ms or 100

Harness.info("PLC:      " .. tostring(Config.plc.host) .. ":" .. tostring(Config.plc.port))
Harness.info("Mode:     " .. tostring(Config.plc.address_mode))
Harness.info("Outputs:  " .. #lockers .. " of " .. #Config.Lockers() .. " doors")
Harness.info("Pulse:    " .. holdMs .. " ms")

for _, range in ipairs(Config.BulkRanges()) do
    if range.space == "coil" then
        Harness.info("Run:      " .. range.address .. "-" .. (range.address + range.count - 1) ..
                     "  (" .. range.count .. " coils, one block read)")
    end
end

if #lockers == 0 then
    Harness.check("lockers with an unlock output are configured", false,
                  "every door in locker.blocks is missing its output mapping")
    Harness.finish()
    return
end

------------------------------------------------------------
-- Connect and read the resting state
------------------------------------------------------------

Harness.section("Connection")

local connected, connectError = Modbus.connect()

if not Harness.check("PLC is reachable", connected, connectError) then
    Harness.finish()
    return
end

Harness.check("every configured address is in the space its block declares", (Modbus.check_mapping()))

local function readAll()

    Modbus.begin_scan()

    local state = {}

    for _, locker in ipairs(lockers) do
        state[locker.locker_id] = Modbus.read_register_bit(locker.output_register, locker.output_bit)
    end

    return state

end

Harness.section("Resting state")

local initial = readAll()
local stuck = {}

for _, locker in ipairs(lockers) do
    if initial[locker.locker_id] == true then
        stuck[#stuck + 1] = locker.locker_id
    end
end

-- A latch already standing high is a fault in its own right.
Harness.check("no unlock output is stuck ON at rest", #stuck == 0,
              #stuck > 0 and ("locker(s) " .. table.concat(stuck, ", ") .. " are already energised") or nil)

------------------------------------------------------------
-- Pulse each output
------------------------------------------------------------

Harness.section("Pulsing " .. #lockers .. " outputs")

for _, locker in ipairs(lockers) do

    local before = readAll()

    local pulsed, pulseError = Modbus.trigger_bit(locker.output_register, locker.output_bit, holdMs)

    Harness.check(string.format("locker %-2d  pulse %s", locker.locker_id,
                                Modbus.describe(locker.output_register, locker.output_bit)),
                  pulsed == true, pulseError)

    local after = readAll()

    if after[locker.locker_id] == nil then

        Harness.skip(string.format("locker %d -- readback", locker.locker_id), "output could not be read")

    else

        Harness.check(string.format("locker %-2d  returned to 0", locker.locker_id),
                      after[locker.locker_id] == false,
                      "output is still energised")

        -- Rule 10, and the duplicate-address check.
        local disturbed = {}

        for _, other in ipairs(lockers) do
            if other.locker_id ~= locker.locker_id
               and before[other.locker_id] ~= nil
               and before[other.locker_id] ~= after[other.locker_id] then
                disturbed[#disturbed + 1] = other.locker_id
            end
        end

        Harness.check(string.format("locker %-2d  no other output moved", locker.locker_id),
                      #disturbed == 0,
                      #disturbed > 0 and ("locker(s) " .. table.concat(disturbed, ", ") ..
                                          " changed too -- check for a duplicated address") or nil)

    end

    Sleep(500)

end

Harness.info("")
Harness.info("Checklist for Plan section 19:")
Harness.info("  [ ] each pulse released exactly one latch")
Harness.info("  [ ] the latch that opened is the locker number the line names --")
Harness.info("      a door that opens out of order means the segments are wrong")
Harness.info("  [ ] " .. holdMs .. " ms is long enough for this lock (raise locker.lock_trigger_ms if not)")

Harness.finish()
