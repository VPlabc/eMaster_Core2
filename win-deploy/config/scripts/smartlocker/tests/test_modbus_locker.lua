-- tests/test_modbus_locker.lua
-- Stage 1.3 (Plan section 20): the full per-locker cycle.
--
--     read door -> unlock -> wait OPEN -> wait CLOSE -> PASS/FAIL
--
-- This is the test that proves the MAPPING: that locker 3's output really does
-- open the door whose sensor is locker 3's input. A cabinet wired one position
-- out passes both of the previous tests and fails this one on every door.
--
-- Someone has to stand at the cabinet: each locker waits for the door to be
-- opened and closed by hand.

local ok = pcall(require, "bootstrap")
if not ok then require("smartlocker.tests.bootstrap") end

local Config   = require("config")
local Database = require("database.database")
local Harness  = require("tests.harness")
local Locker   = require("core.locker")
local LockerDb = require("database.locker_db")
local Modbus   = require("hardware.modbus")

-- Set this to a single locker number to test just one; nil tests all of them.
local ONLY_LOCKER = nil

local OPEN_TIMEOUT = Config.locker.door_open_timeout or 30
local CLOSE_TIMEOUT = Config.locker.door_close_timeout or 30

Harness.start("Stage 1.3 -- locker hardware")

------------------------------------------------------------
-- Setup
------------------------------------------------------------

-- The layout comes from the database, and the test database is used so this
-- never touches live assignments.
local opened, dbError = Database.open(Harness.TEST_DATABASE)

if not Harness.check("test database", opened, dbError) then
    Harness.finish()
    return
end

Database.reset()
Locker.init()

local connected, connectError = Modbus.connect()

if not Harness.check("PLC is reachable", connected, connectError) then
    Harness.finish()
    return
end

------------------------------------------------------------
-- Cycle one locker
------------------------------------------------------------

local function testLocker(locker)

    Harness.section("Locker " .. locker.locker_number .. " (" .. tostring(locker.locker_type) .. ")")

    Harness.info("Input:  " .. Modbus.describe(locker.input_register, locker.input_bit))

    if not LockerDb.can_unlock(locker) then
        Harness.skip("locker " .. locker.locker_number, "no unlock output configured (Plan section 6)")
        return
    end

    Harness.info("Output: " .. Modbus.describe(locker.output_register, locker.output_bit))
    Harness.info("Pulse:  " .. (Config.locker.lock_trigger_ms or 100) .. " ms")

    Modbus.begin_scan()

    local startState = Locker.is_open(locker.id)

    if not Harness.check("door state readable", startState ~= nil) then
        return
    end

    if startState then
        Harness.skip("locker " .. locker.locker_number, "the door is already open -- close it and run again")
        return
    end

    Harness.info("Door starts CLOSED. Unlocking...")

    local unlocked, unlockError = Locker.unlock(locker.id, "TEST")

    if not Harness.check("unlock pulse sent", unlocked, unlockError) then
        return
    end

    local didOpen, openError = Locker.wait_open(locker.id, OPEN_TIMEOUT)

    if not Harness.check("door OPENED within " .. OPEN_TIMEOUT .. "s", didOpen, openError) then
        Harness.info("   Check: is this output wired to THIS door? Is the pulse long enough?")
        Locker.reset_runtime()
        return
    end

    Harness.info("Door is OPEN. Close it.")

    local didClose, closeError = Locker.wait_close(locker.id, CLOSE_TIMEOUT)

    Harness.check("door CLOSED within " .. CLOSE_TIMEOUT .. "s", didClose, closeError)

    -- The runtime entry left behind by unlock() is cleared here because this
    -- test drives the doors with the blocking helpers rather than the state
    -- machine.
    Locker.reset_runtime()

    Harness.check("locker " .. locker.locker_number .. " CLOSED -> OPEN -> CLOSED", didOpen and didClose)

end

------------------------------------------------------------
-- Run
------------------------------------------------------------

local lockers = LockerDb.all()

Harness.info(#lockers .. " locker(s) configured")

for _, locker in ipairs(lockers) do

    if ONLY_LOCKER == nil or locker.locker_number == ONLY_LOCKER then
        testLocker(locker)
    end

end

Harness.info("")
Harness.info("Checklist for Plan section 20:")
Harness.info("  [ ] the door that opened is the door named in the section header")
Harness.info("  [ ] no OTHER door opened during any cycle")
Harness.info("  [ ] lockers with no output were skipped, not failed")

Harness.finish()
