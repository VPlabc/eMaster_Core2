-- hardware/modbus.lua
-- The only file in this project that talks to the PLC (Plan sections 26 and
-- 39.3). Everything above it deals in "locker 3's door bit", never in function
-- codes or register offsets.
--
-- ADDRESS TRANSLATION. The plan specifies logical Modicon addresses -- input
-- register 30001, holding register 40001 -- and says the driver converts them
-- (section 5.2). The gateway's native Modbus bindings take PROTOCOL offsets:
-- ModbusClient writes the number it is given straight into the PDU, with no
-- 4xxxx arithmetic anywhere. So 40001 becomes offset 0 of the holding-register
-- space here, and nothing else in the project has to know that.
--
-- This cabinet is not wired the way the plan assumed. Its doors are not packed
-- sixteen to a word: every lock relay is its OWN coil, numbered from 10001, and
-- every door sensor its OWN discrete input, numbered from 20001, across the
-- PLC's internal I/O and its expansion module. Those two ranges are what
-- `address_mode = "locker_io"` adds below; 30001/40001 keep their Modicon
-- meaning, so a word-addressed device on the same PLC is untouched.
--
-- WHICH CONNECTION. Reads and writes go through the gateway's own configured
-- PLC connection (the `modbus` section on the Configuration page). That is not
-- a preference: Modbus.WriteHolding/ReadHolding have no ip/port form in
-- LuaEngine -- only ReadCoil, ReadDiscreteInput and ReadInputRegister do -- so
-- the configured connection is the only path that can write a holding register
-- at all. plc.host/plc.port in smartlocker.json are checked against it at
-- start-up and a mismatch is reported.

local Gateway = Modbus

local Config = require("config")
local Logger = require("utils.logger")

local Modbus = {}

local settings = Config.plc

------------------------------------------------------------
-- Address spaces
------------------------------------------------------------

Modbus.SPACE = {
    COIL           = "coil",            -- FC01 / FC05, read-write bits
    DISCRETE_INPUT = "discrete_input",  -- FC02, read-only bits
    INPUT_REGISTER = "input_register",  -- FC04, read-only words
    HOLDING        = "holding",         -- FC03 / FC06, read-write words
}

-- The two spaces that address one bit each. A door sensor and a lock relay live
-- in these; a packed door register does not.
local BIT_SPACES = {
    [Modbus.SPACE.COIL] = true,
    [Modbus.SPACE.DISCRETE_INPUT] = true,
}

function Modbus.is_bit_space(space)
    return BIT_SPACES[space] == true
end

-- Address schemes, selected by plc.address_mode. Each is a list of logical
-- ranges in 5- and 6-digit form; an address outside every range of the selected
-- scheme is already a protocol offset and keeps `defaultSpace`.
--
--   "modicon"   the classic map -- 00001 coils, 10001 discrete inputs,
--               30001 input registers, 40001 holding registers.
--   "locker_io" THIS cabinet -- 10001 is the first lock relay COIL (Y0) and
--               20001 the first door sensor DISCRETE INPUT (X0). The 1xxxx
--               range therefore means the opposite of what it means under
--               Modicon, which is exactly why it is a separate scheme rather
--               than an extra range bolted onto one shared table.
--   anything else (including "raw") -- no translation at all.
local SCHEMES = {

    modicon = {
        { first = 1,      last = 9999,   space = Modbus.SPACE.COIL,           base = 1 },
        { first = 10001,  last = 19999,  space = Modbus.SPACE.DISCRETE_INPUT, base = 10001 },
        { first = 30001,  last = 39999,  space = Modbus.SPACE.INPUT_REGISTER, base = 30001 },
        { first = 40001,  last = 49999,  space = Modbus.SPACE.HOLDING,        base = 40001 },
        { first = 100001, last = 165536, space = Modbus.SPACE.COIL,           base = 100001 },
        { first = 300001, last = 365536, space = Modbus.SPACE.INPUT_REGISTER, base = 300001 },
        { first = 400001, last = 465536, space = Modbus.SPACE.HOLDING,        base = 400001 },
    },

    locker_io = {
        { first = 1,      last = 9999,   space = Modbus.SPACE.COIL,           base = 1 },
        { first = 10001,  last = 19999,  space = Modbus.SPACE.COIL,           base = 10001 },
        { first = 20001,  last = 29999,  space = Modbus.SPACE.DISCRETE_INPUT, base = 20001 },
        { first = 30001,  last = 39999,  space = Modbus.SPACE.INPUT_REGISTER, base = 30001 },
        { first = 40001,  last = 49999,  space = Modbus.SPACE.HOLDING,        base = 40001 },
        { first = 100001, last = 165536, space = Modbus.SPACE.COIL,           base = 100001 },
        { first = 200001, last = 265536, space = Modbus.SPACE.DISCRETE_INPUT, base = 200001 },
        { first = 300001, last = 365536, space = Modbus.SPACE.INPUT_REGISTER, base = 300001 },
        { first = 400001, last = 465536, space = Modbus.SPACE.HOLDING,        base = 400001 },
    },

}

-- plc.address_ranges, when present, replaces the scheme outright -- for a PLC
-- whose manual publishes a map neither of the two above describes.
local function ranges()

    if type(settings.address_ranges) == "table" and #settings.address_ranges > 0 then
        return settings.address_ranges
    end

    return SCHEMES[settings.address_mode]

end

-- Modbus.decode(30003) -> "input_register", 2
-- Modbus.decode(20007) -> "discrete_input", 6      (locker_io)
-- Modbus.decode(10007) -> "coil", 6                (locker_io)
function Modbus.decode(address, defaultSpace)

    local space = defaultSpace or Modbus.SPACE.HOLDING
    local scheme = ranges()

    if scheme == nil then
        return space, address
    end

    for _, range in ipairs(scheme) do
        if address >= range.first and address <= range.last then
            return range.space, address - range.base
        end
    end

    return space, address

end

-- The reverse, for messages a person has to read.
function Modbus.describe(address, bit)

    local space, offset = Modbus.decode(address)
    local text = tostring(address) .. " (" .. space .. " offset " .. tostring(offset) .. ")"

    -- A coil or a discrete input IS the bit; saying "bit 0" about it is noise
    -- that reads as though there were sixteen of them.
    if bit ~= nil and not (Modbus.is_bit_space(space) and bit == 0) then
        text = text .. " bit " .. tostring(bit)
    end

    return text

end

------------------------------------------------------------
-- Statistics and health
------------------------------------------------------------

local stats = {
    reads = 0,
    writes = 0,
    errors = 0,
    consecutive_errors = 0,
    last_error = nil,
    connected = nil,
}

function Modbus.stats()
    return stats
end

local function failed(operation, address, message)

    stats.errors = stats.errors + 1
    stats.consecutive_errors = stats.consecutive_errors + 1
    stats.last_error = message

    -- Logged as a business event only on the transition into failure, and again
    -- every 50 failures while it lasts. An unreachable PLC produces several
    -- failed reads per second; one row per read would bury every other event on
    -- the Logs page within minutes.
    if stats.consecutive_errors == 1 or stats.consecutive_errors % 50 == 0 then
        Logger.plc_error(operation, address, message)
    end

    return nil, message

end

local function succeeded()

    if stats.consecutive_errors > 0 then
        Logger.info("PLC communication recovered after " .. stats.consecutive_errors .. " failed operations")
    end

    stats.consecutive_errors = 0

end

------------------------------------------------------------
-- Connection
------------------------------------------------------------

-- Every configured address must decode into the space its block declared. This
-- is the mistake plc.address_mode invites: under "modicon" the lock relay at
-- 10001 decodes as a *discrete input*, so every unlock is aimed at a read-only
-- space and the only symptom is doors that never open. Reported, not fatal --
-- an operator with a half-migrated configuration should still get a running
-- gateway and a message naming the door.
function Modbus.check_mapping()

    local problems = {}

    for _, locker in ipairs(Config.Lockers()) do

        local checks = {
            { address = locker.input_register,  want = locker.input_space,  what = "input" },
            { address = locker.output_register, want = locker.output_space, what = "output" },
        }

        for _, check in ipairs(checks) do

            if check.address ~= nil and check.want ~= nil then

                local space = Modbus.decode(check.address)

                if space ~= check.want then
                    problems[#problems + 1] = string.format(
                        "locker %d %s address %d reads as %s, but the block declares %s",
                        locker.locker_id, check.what, check.address, space, check.want)
                end

            end

        end

    end

    for _, problem in ipairs(problems) do
        Logger.warning("PLC mapping: " .. problem ..
                       " -- check plc.address_mode (currently " .. tostring(settings.address_mode) .. ")")
    end

    return #problems == 0, problems

end

-- There is nothing to open: the gateway owns the socket and reconnects on its
-- own. This checks that the PLC is actually reachable and that the gateway is
-- pointed at the same one this project is configured for -- a mismatch would
-- otherwise show up as lockers that never open, with every read succeeding
-- against the wrong panel.
function Modbus.connect()

    Modbus.check_mapping()

    local host = settings.host
    local port = settings.port or 502

    local gatewayHost = Config.GatewayValue("modbus.ip", nil)
    local gatewayPort = Config.GatewayValue("modbus.port", nil)

    if settings.use_gateway_connection and gatewayHost and host and gatewayHost ~= host then
        Logger.warning("plc.host is " .. tostring(host) .. " but the gateway's Modbus connection points at " ..
                       tostring(gatewayHost) .. " -- reads and writes go to the GATEWAY's PLC. " ..
                       "Fix the Configuration page or plc.host so the two agree.")
    end

    if settings.use_gateway_connection and gatewayPort and port and gatewayPort ~= port then
        Logger.warning("plc.port is " .. tostring(port) .. " but the gateway's Modbus port is " ..
                       tostring(gatewayPort))
    end

    local reachable = false

    if type(Gateway.CheckConnect) == "function" and host then
        reachable = Gateway.CheckConnect(host, port) == true
    end

    stats.connected = reachable

    if not reachable then
        return false, "PLC " .. tostring(host) .. ":" .. tostring(port) .. " is not reachable"
    end

    return true

end

function Modbus.disconnect()
    -- The gateway owns the connection; a script must not close it out from
    -- under the poll thread. Kept so the abstraction of Plan section 26 is
    -- complete and callers do not grow an exception for it.
    stats.connected = nil
    return true
end

function Modbus.is_connected()

    if type(Gateway.IsConnected) == "function" then
        return Gateway.IsConnected() == true
    end

    return stats.connected == true

end

------------------------------------------------------------
-- Read cache
------------------------------------------------------------

-- Door polling asks about one door at a time, and one Modbus reply covers many
-- of them -- sixteen doors in a packed register, a whole module in a block read
-- (below). Within a scan every read is answered from the first one that fetched
-- it, so polling a 36-door cabinet costs a handful of round trips instead of
-- thirty-six. Modbus.begin_scan() opens the next scan; the main loop calls it
-- once per pass.
local cache = {}
local cacheEnabled = settings.read_cache ~= false

function Modbus.begin_scan()
    cache = {}
end

function Modbus.set_cache_enabled(enabled)
    cacheEnabled = enabled ~= false
    cache = {}
end

local function cacheKey(space, offset)
    return space .. ":" .. tostring(offset)
end

------------------------------------------------------------
-- Bulk reads
------------------------------------------------------------

-- One coil per lock and one discrete input per door means the scan cache above
-- has nothing left to share: sixteen doors in one register was what made it
-- cheap, and eighteen doors at eighteen addresses is eighteen round trips a
-- pass. So a bit-space read that misses the cache pulls the WHOLE contiguous
-- run its address belongs to in one FC01/FC02 and fills the cache with it --
-- back to one round trip per module per scan, and a module nothing asked about
-- this pass still costs nothing.
--
-- The runs come from the configured layout (Config.bulk_ranges, built by
-- config.lua from the same segments the lockers are mapped from), so a gap
-- between the internal I/O and an expansion module is two runs and never one
-- read straddling addresses the PLC does not answer for.
--
-- Only the gateway's own connection can do this: Modbus.ReadCoils and
-- Modbus.ReadDiscreteInputs have no ip/port form.
local bulkRanges = nil

-- Overrides the configured runs; called with nothing, goes back to them.
function Modbus.set_bulk_ranges(list)
    bulkRanges = list
    cache = {}
end

-- The configured runs, unless a caller (a test) has replaced them. Read through
-- to Config on every call rather than captured once, so Config.Reload() moves
-- the block reads with the layout.
function Modbus.bulk_ranges()

    if bulkRanges ~= nil then
        return bulkRanges
    end

    return Config.BulkRanges()

end

local function bulkFill(space, offset)

    if not cacheEnabled or not settings.use_gateway_connection then
        return nil
    end

    local reader

    if space == Modbus.SPACE.COIL then
        reader = Gateway.ReadCoils
    elseif space == Modbus.SPACE.DISCRETE_INPUT then
        reader = Gateway.ReadDiscreteInputs
    end

    if type(reader) ~= "function" then
        return nil
    end

    for _, range in ipairs(Modbus.bulk_ranges()) do

        local rangeSpace, rangeOffset = Modbus.decode(range.address, range.space)
        local count = range.count or 0

        if rangeSpace == space and count > 1
           and offset >= rangeOffset and offset < rangeOffset + count then

            local values = reader(rangeOffset, count)

            if type(values) ~= "table" then
                -- The PLC refused the block read (a run wider than it allows,
                -- or an address inside it that does not exist). Say nothing and
                -- let the caller fall back to the single read: one door that
                -- cannot be read must not take the other seventeen with it.
                return nil
            end

            for index = 1, count do
                local value = values[index]
                if value ~= nil then
                    cache[cacheKey(space, rangeOffset + index - 1)] = value and 1 or 0
                end
            end

            return values[offset - rangeOffset + 1]

        end

    end

    return nil

end

------------------------------------------------------------
-- Primitive operations
------------------------------------------------------------

local function callWithRetry(operation, address, fn)

    local attempts = (settings.retry_count or 0) + 1

    for attempt = 1, attempts do

        local value = fn()

        if value ~= nil then
            succeeded()
            return value
        end

        if attempt < attempts then
            Sleep(settings.retry_delay_ms or 50)
        end

    end

    return failed(operation, address, "no reply from PLC")

end

local function readRaw(space, offset)

    local host, port = settings.host, settings.port
    local direct = not settings.use_gateway_connection

    if space == Modbus.SPACE.HOLDING then
        -- No ip/port form exists for holding registers; the gateway's
        -- connection is the only way in.
        return Gateway.ReadHolding(offset)
    end

    if space == Modbus.SPACE.INPUT_REGISTER then
        if direct then
            return Gateway.ReadInputRegister(host, port, offset)
        end
        return Gateway.ReadInputRegister(offset)
    end

    if space == Modbus.SPACE.COIL then
        if direct then
            return Gateway.ReadCoil(host, port, offset)
        end
        return Gateway.ReadCoil(offset)
    end

    if space == Modbus.SPACE.DISCRETE_INPUT then
        if direct then
            return Gateway.ReadDiscreteInput(host, port, offset)
        end
        return Gateway.ReadDiscreteInput(offset)
    end

    return nil

end

-- Modbus.read_register(address [, space]) -> value | nil, error
function Modbus.read_register(address, space)

    local resolvedSpace, offset = Modbus.decode(address, space or Modbus.SPACE.HOLDING)
    local key = cacheKey(resolvedSpace, offset)

    if cacheEnabled and cache[key] ~= nil then
        return cache[key]
    end

    stats.reads = stats.reads + 1

    -- A door sensor or a lock relay: try to bring in its whole module at once.
    if Modbus.is_bit_space(resolvedSpace) then

        local bulk = bulkFill(resolvedSpace, offset)

        if bulk ~= nil then
            succeeded()
            return bulk and 1 or 0
        end

    end

    local value, err = callWithRetry("read_register", address, function()
        return readRaw(resolvedSpace, offset)
    end)

    if value == nil then
        return nil, err
    end

    -- Booleans come back from the bit spaces; normalise so a caller that asked
    -- for a "register" always gets a number.
    if type(value) == "boolean" then
        value = value and 1 or 0
    end

    if cacheEnabled then
        cache[key] = value
    end

    return value

end

-- Modbus.write_register(address, value [, space]) -> ok, error
function Modbus.write_register(address, value, space)

    local resolvedSpace, offset = Modbus.decode(address, space or Modbus.SPACE.HOLDING)

    stats.writes = stats.writes + 1

    local ok

    if resolvedSpace == Modbus.SPACE.COIL then

        local bit = value ~= 0 and value ~= false

        if not settings.use_gateway_connection then
            ok = Gateway.WriteCoil(settings.host, settings.port, offset, bit)
        else
            ok = Gateway.WriteCoil(offset, bit)
        end

    elseif resolvedSpace == Modbus.SPACE.HOLDING then
        ok = Gateway.WriteHolding(offset, value)
    else
        return failed("write_register", address, resolvedSpace .. " is read-only")
    end

    -- The written value invalidates whatever the cache believed.
    cache[cacheKey(resolvedSpace, offset)] = nil

    if ok ~= true then
        return failed("write_register", address, "PLC rejected the write")
    end

    succeeded()

    return true

end

------------------------------------------------------------
-- Bit operations
------------------------------------------------------------

-- Bit 0 is the least significant bit of the register, which is how the plan's
-- table reads: bits 8-15 of 30001 are the high byte, the eight door sensors.
--
-- A coil or discrete input carries one bit and the caller has nothing to pick,
-- so `bit` is optional there -- and bit 0 of the 1/0 a bit-space read returns is
-- that same bit, which is why one code path serves both shapes.
function Modbus.read_register_bit(address, bit, space)

    bit = bit or 0

    local value, err = Modbus.read_register(address, space)

    if value == nil then
        return nil, err
    end

    return (value & (1 << bit)) ~= 0

end

-- Read-modify-write. Plan section 5.2 and rule 10: the other bits in that
-- register drive other lockers and must survive untouched, so the current value
-- is read back immediately before the write rather than tracked in a shadow
-- copy that could drift from the PLC.
--
-- `busy` is a re-entrancy guard, not a thread lock. Only one thread runs a
-- script, but Sleep() -- which is where the unlock pulse spends its 100 ms -- is
-- exactly where the engine delivers queued events (OnMqMessage, ZK card
-- callbacks). A handler firing there can call straight back into this module, so
-- the guard is held across the WHOLE pulse, not just across one write: an
-- interleaved read-modify-write would read the register while this bit is high
-- and write that back after it should have gone low, leaving a latch energised.
local busy = false

local function writeBit(address, bit, value, space)

    local resolvedSpace = space or (Modbus.decode(address))

    -- Coils address one bit each -- there is nothing to preserve and no read to
    -- do. Only word spaces need the read-modify-write.
    if resolvedSpace == Modbus.SPACE.COIL then
        return Modbus.write_register(address, value and 1 or 0, resolvedSpace)
    end

    -- Read straight from the PLC, never from the scan cache: a cached value is
    -- up to one main-loop pass old and would write back a stale picture of the
    -- other fifteen doors.
    local resolved, offset = Modbus.decode(address, resolvedSpace)
    cache[cacheKey(resolved, offset)] = nil

    local current, err = Modbus.read_register(address, resolvedSpace)

    if current == nil then
        return nil, err
    end

    local updated

    if value then
        updated = current | (1 << bit)
    else
        updated = current & (~(1 << bit) & 0xFFFF)
    end

    if updated == current then
        return true
    end

    return Modbus.write_register(address, updated, resolvedSpace)

end

function Modbus.write_register_bit(address, bit, value, space)

    bit = bit or 0

    if busy then
        return nil, "another register update is in progress"
    end

    busy = true

    local ok, err = writeBit(address, bit, value, space)

    busy = false

    return ok, err

end

-- Set, hold, clear -- the unlock pulse of Plan section 5.2. The clear is
-- attempted even when the hold went wrong, because a latch left energised is a
-- locker that never locks again.
function Modbus.trigger_bit(address, bit, durationMs, space)

    bit = bit or 0

    if busy then
        return nil, "another register update is in progress"
    end

    busy = true

    local ok, err = writeBit(address, bit, true, space)

    if not ok then
        busy = false
        return nil, err
    end

    Sleep(durationMs or 100)

    local cleared, clearErr = writeBit(address, bit, false, space)

    busy = false

    if not cleared then
        -- Loud: the output is still SET, so the door is held unlocked and no
        -- amount of business logic above this can tell.
        Logger.plc_error("trigger_bit_clear", address,
                         "output stayed SET: " .. tostring(clearErr))
        return nil, clearErr
    end

    return true

end

------------------------------------------------------------
-- Bulk helpers (tests and diagnostics)
------------------------------------------------------------

function Modbus.to_hex(value)
    return string.format("0x%04X", value or 0)
end

function Modbus.to_binary(value)

    local bits = {}

    for bit = 15, 0, -1 do
        bits[#bits + 1] = ((value & (1 << bit)) ~= 0) and "1" or "0"
        if bit % 4 == 0 and bit > 0 then
            bits[#bits + 1] = " "
        end
    end

    return table.concat(bits)

end

return Modbus
