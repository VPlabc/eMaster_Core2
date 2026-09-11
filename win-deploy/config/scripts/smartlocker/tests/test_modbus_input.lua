-- tests/test_modbus_input.lua
-- Stage 1.1 (Plan section 18): read every door sensor and watch it move.
--
-- Two things are verified here, and neither can be read off a wiring diagram:
--
--   SENSOR POLARITY. The default is 1 = OPEN and section 5.1 says explicitly
--   that it has to be checked. Open one door by hand and watch which way its
--   input moves; if a door reads OPEN with every door shut, set
--   plc.door_open_value to 0 in smartlocker.json and nothing else changes.
--
--   THE EXPANSION MODULE'S BASE ADDRESS. Doors 1-6 are on the PLC's own I/O and
--   the rest on the module after it, at an address this project has to be told.
--   A wrong base is silent -- reads succeed and answer about nothing -- so the
--   symptom to look for is a whole run of doors that never changes while the
--   first six do.
--
-- Doors are walked from the configured layout, so the same test covers a
-- bit-addressed cabinet and a register-packed one.

local ok = pcall(require, "bootstrap")
if not ok then require("smartlocker.tests.bootstrap") end

local Config  = require("config")
local Harness = require("tests.harness")
local Modbus  = require("hardware.modbus")

-- How long to sit and watch for changes, in seconds.
local MONITOR_SECONDS = 30

Harness.start("Stage 1.1 -- door sensor inputs")

local lockers = Config.Lockers()

Harness.info("PLC:      " .. tostring(Config.plc.host) .. ":" .. tostring(Config.plc.port))
Harness.info("Mode:     " .. tostring(Config.plc.address_mode))
Harness.info("Doors:    " .. #lockers)
Harness.info("Polarity: " .. ((Config.plc.door_open_value or 1) == 1 and "1 = OPEN" or "0 = OPEN"))

for _, range in ipairs(Config.BulkRanges()) do
    if range.space == "discrete_input" then
        Harness.info("Run:      " .. range.address .. "-" .. (range.address + range.count - 1) ..
                     "  (" .. range.count .. " points, one block read)")
    end
end

if #lockers == 0 then
    Harness.check("lockers are configured", false, "locker.blocks is empty")
    Harness.finish()
    return
end

------------------------------------------------------------
-- Connect
------------------------------------------------------------

Harness.section("Connection")

local connected, connectError = Modbus.connect()

Harness.check("PLC is reachable", connected, connectError)

if not connected then
    Harness.info("Nothing further can be tested without the PLC.")
    Harness.finish()
    return
end

Harness.check("gateway Modbus connection is up", Modbus.is_connected())
Harness.check("every configured address is in the space its block declares", (Modbus.check_mapping()))

------------------------------------------------------------
-- Read every door
------------------------------------------------------------

Harness.section("Reading " .. #lockers .. " door sensors")

-- Reads the whole cabinet in one scan: the block reads above mean this costs
-- one round trip per I/O module, not one per door.
local function readAll()

    Modbus.begin_scan()

    local state = {}

    for _, locker in ipairs(lockers) do

        if locker.input_register ~= nil then

            local raw = Modbus.read_register_bit(locker.input_register, locker.input_bit)

            if raw ~= nil then
                local open = raw
                if (Config.plc.door_open_value or 1) == 0 then
                    open = not raw
                end
                state[locker.locker_id] = { raw = raw, open = open }
            end

        end

    end

    return state

end

local state = readAll()

for _, locker in ipairs(lockers) do

    local entry = state[locker.locker_id]

    if locker.input_register == nil then
        Harness.skip("locker " .. locker.locker_id, "no input mapping")
    else
        Harness.check(string.format("locker %-2d  %s", locker.locker_id,
                                    Modbus.describe(locker.input_register, locker.input_bit)),
                      entry ~= nil,
                      entry and string.format("raw=%s  door=%s", tostring(entry.raw),
                                              entry.open and "OPEN" or "CLOSED")
                             or "no reply")
    end

end

------------------------------------------------------------
-- Monitor for changes
------------------------------------------------------------

Harness.section("Monitoring for " .. MONITOR_SECONDS .. "s -- open and close each door by hand")

local moved = {}
local changes = 0
local elapsed = 0

while elapsed < MONITOR_SECONDS * 1000 do

    Sleep(200)
    elapsed = elapsed + 200

    local current = readAll()

    for _, locker in ipairs(lockers) do

        local was = state[locker.locker_id]
        local now = current[locker.locker_id]

        if was ~= nil and now ~= nil and was.open ~= now.open then

            changes = changes + 1
            moved[locker.locker_id] = true

            Harness.info(string.format("   locker %d (%d): %s", locker.locker_id,
                                       locker.input_register, now.open and "OPEN" or "CLOSED"))

        end

    end

    state = current

end

Harness.check("state changes were detected", changes > 0,
              changes .. " change(s) -- zero means either no door was moved, or the sensors are " ..
              "not at the configured addresses")

-- Which doors never moved is the reading that catches a wrong expansion base:
-- one contiguous run of silent doors is a segment pointed at the wrong address.
local silent = {}

for _, locker in ipairs(lockers) do
    if not moved[locker.locker_id] then
        silent[#silent + 1] = locker.locker_id
    end
end

if #silent > 0 then
    Harness.info("No change seen on locker(s): " .. table.concat(silent, ", "))
end

Harness.info("")
Harness.info("Checklist for Plan section 18:")
Harness.info("  [ ] every door has its own input and no two move together")
Harness.info("  [ ] the polarity matches plc.door_open_value")
Harness.info("  [ ] doors on the expansion module respond -- a whole silent run")
Harness.info("      means the second segment's address is wrong")

Harness.finish()
