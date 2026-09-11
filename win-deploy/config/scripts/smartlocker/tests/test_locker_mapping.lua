-- tests/test_locker_mapping.lua
-- Prints the address array the configured layout expands to, and checks it for
-- the mistakes that are silent on the wire.
--
-- No PLC and no database: this reads config.lua only, so it is the first thing
-- to run after editing locker.blocks -- before test_modbus_input, which needs
-- the cabinet, and long before anything opens a door.
--
-- What it catches:
--   * a door mapped to no address, or two doors mapped to the SAME address
--     (two lockers that open together, which on the wire looks like a working
--     unlock and an unrelated broken one)
--   * an address that decodes into a different space than its block declares --
--     the plc.address_mode mistake, where a lock relay coil at 10001 is read as
--     a discrete input and every unlock is written to a read-only space
--   * a segment list that does not add up to the block's door count
--   * the block-read runs, which is where a wrong expansion base is visible as
--     a run starting at an address nobody wired

local ok = pcall(require, "bootstrap")
if not ok then require("smartlocker.tests.bootstrap") end

local Config  = require("config")
local Harness = require("tests.harness")
local Modbus  = require("hardware.modbus")

Harness.start("Locker address mapping")

local lockers = Config.Lockers()

Harness.info("Source:   " .. tostring(Config.SourcePath() or "defaults only"))
Harness.info("Mode:     " .. tostring(Config.plc.address_mode))
Harness.info("Doors:    " .. #lockers)

if Config.LoadError() then
    Harness.check("configuration parsed", false, Config.LoadError())
end

if not Harness.check("lockers are configured", #lockers > 0,
                     #lockers == 0 and "locker.blocks is empty" or nil) then
    Harness.finish()
    return
end

------------------------------------------------------------
-- Blocks
------------------------------------------------------------

Harness.section("Blocks")

for _, block in ipairs(Config.Blocks()) do
    Harness.info(string.format("block %d  %-12s %-11s %2d doors, %2d with an unlock output",
                               block.id, block.name, block.type, block.count, block.output_count or 0))
end

------------------------------------------------------------
-- The array
------------------------------------------------------------

Harness.section("Address array")

Harness.info(string.format("%-4s %-10s %-24s %s", "id", "type", "input (door sensor)", "output (lock relay)"))

local function cell(address, bit, space)

    if address == nil then
        return "-"
    end

    local decoded = Modbus.decode(address)
    local text = tostring(address) .. " " .. decoded

    if not Modbus.is_bit_space(decoded) then
        text = text .. " bit " .. tostring(bit)
    end

    if space ~= nil and space ~= decoded then
        text = text .. "  <- declared " .. space
    end

    return text

end

for _, locker in ipairs(lockers) do
    Harness.info(string.format("%-4d %-10s %-24s %s",
                               locker.locker_id, locker.locker_type,
                               cell(locker.input_register, locker.input_bit, locker.input_space),
                               cell(locker.output_register, locker.output_bit, locker.output_space)))
end

------------------------------------------------------------
-- Checks
------------------------------------------------------------

Harness.section("Checks")

local missingInput, duplicateInput = {}, {}
local duplicateOutput = {}
local seenInput, seenOutput = {}, {}

for _, locker in ipairs(lockers) do

    local input = locker.input_register

    if input == nil then
        missingInput[#missingInput + 1] = locker.locker_id
    else
        local key = tostring(locker.input_space) .. ":" .. input
        if seenInput[key] then
            duplicateInput[#duplicateInput + 1] = seenInput[key] .. "/" .. locker.locker_id .. "@" .. input
        else
            seenInput[key] = locker.locker_id
        end
    end

    local output = locker.output_register

    if output ~= nil then
        local key = tostring(locker.output_space) .. ":" .. output
        if seenOutput[key] then
            duplicateOutput[#duplicateOutput + 1] = seenOutput[key] .. "/" .. locker.locker_id .. "@" .. output
        else
            seenOutput[key] = locker.locker_id
        end
    end

end

-- A door with no sensor cannot be timed out, so it is a layout error rather
-- than a wiring choice. A door with no unlock output IS a wiring choice
-- (section 6) and is only reported.
Harness.check("every door has an input address", #missingInput == 0,
              #missingInput > 0 and ("locker(s) " .. table.concat(missingInput, ", ")) or nil)

Harness.check("no two doors share an input address", #duplicateInput == 0,
              #duplicateInput > 0 and table.concat(duplicateInput, ", ") or nil)

Harness.check("no two doors share an output address", #duplicateOutput == 0,
              #duplicateOutput > 0 and table.concat(duplicateOutput, ", ") or nil)

local unwired = 0

for _, locker in ipairs(lockers) do
    if locker.output_register == nil then
        unwired = unwired + 1
    end
end

if unwired > 0 then
    Harness.info(unwired .. " door(s) have no unlock output and will never be assigned")
end

local mapped, problems = Modbus.check_mapping()

Harness.check("every address is in the space its block declares", mapped,
              (not mapped) and table.concat(problems, "; ") or nil)

------------------------------------------------------------
-- Segments and block reads
------------------------------------------------------------

Harness.section("Block reads")

local ranges = Config.BulkRanges()

for _, range in ipairs(ranges) do
    Harness.info(string.format("%-15s %d-%d  (%d points)", range.space,
                               range.address, range.address + range.count - 1, range.count))
end

-- Every address the layout uses has to fall inside one of the runs, or it is
-- read one round trip at a time. Not a fault -- a single door of its own is a
-- legitimate run of one -- but worth seeing.
local covered, uncovered = 0, {}

local function isCovered(space, address)

    if space == nil or address == nil then
        return true
    end

    for _, range in ipairs(ranges) do
        if range.space == space and address >= range.address and address < range.address + range.count then
            return true
        end
    end

    return false

end

for _, locker in ipairs(lockers) do

    for _, pair in ipairs({ { locker.input_space, locker.input_register },
                            { locker.output_space, locker.output_register } }) do

        if pair[2] ~= nil and pair[1] ~= nil then
            if isCovered(pair[1], pair[2]) then
                covered = covered + 1
            else
                uncovered[#uncovered + 1] = pair[2]
            end
        end

    end

end

Harness.info(covered .. " address(es) covered by a block read" ..
             (#uncovered > 0 and (", " .. #uncovered .. " read individually") or ""))

Harness.info("")
Harness.info("Read the array above against the wiring sheet:")
Harness.info("  [ ] locker 1 is the door the panel calls locker 1")
Harness.info("  [ ] the run breaks where the expansion module starts, at the")
Harness.info("      address the PLC program gives it")
Harness.info("  [ ] a door with no output is a door that is genuinely not wired")

Harness.finish()
