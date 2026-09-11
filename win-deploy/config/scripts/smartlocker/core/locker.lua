-- core/locker.lua
-- Lockers: their PLC mapping, their doors, and the state machine of Plan
-- sections 12 and 31.
--
-- PERMANENT vs TEMPORARY STATE. The four permanent states -- EMPTY, ASSIGNED,
-- EXPIRED, ERROR -- describe who a locker belongs to and live in the database.
-- The three temporary ones -- UNLOCKING, OPEN, WAIT_CLOSE -- describe a door in
-- the middle of one swipe and live only in the `runtime` table below. That
-- split is what makes restart recovery honest: after a restart the gateway has
-- no idea whether a door is open, so it asks the PLC (Locker.reconcile) instead
-- of trusting a state it wrote before it died.
--
-- WHY THE DOOR WAIT IS NOT A LOOP. Plan section 8 draws the flow as "unlock ->
-- wait for OPEN -> wait for CLOSE", which reads like two blocking waits. One
-- Lua engine is one thread, and every native binding takes the VM lock: a
-- blocking wait_close() means that for up to 30 seconds this gateway does not
-- read cards, does not run the scheduler, and does not drain RabbitMQ -- and
-- with two people at two cabinets, the second one is simply ignored. So the
-- machine below is driven from Locker.poll(), one pass per loop iteration, and
-- several doors can be in flight at once. Locker.wait_open/wait_close still
-- exist for the hardware tests, which have nothing else to do.

local Config     = require("config")
local EmployeeDb = require("database.employee_db")
local LockerDb   = require("database.locker_db")
local Logger     = require("utils.logger")
local Modbus     = require("hardware.modbus")
local Time       = require("utils.time")

local Locker = {}

Locker.STATUS = LockerDb.STATUS

Locker.RUNTIME = {
    IDLE       = "IDLE",
    UNLOCKING  = "UNLOCKING",
    OPEN       = "OPEN",
    WAIT_CLOSE = "WAIT_CLOSE",
}

-- locker_id -> { state, card_code, since, deadline }
local runtime = {}

-- locker_id -> last door reading, so an edge can be told from a level.
local doorState = {}

-- Mirrors a runtime fact into the database for the dashboard to read. Called
-- ONLY on a change: this runs inside a 100 ms poll loop, and writing the door
-- state every pass would be a few hundred thousand UPDATEs a day on a cabinet
-- where nothing happened.
local function mirrorRuntime(lockerId, state)
    LockerDb.set_runtime_state(lockerId, state)
end

local function mirrorDoor(lockerId, open)
    LockerDb.set_door(lockerId, open)
end

------------------------------------------------------------
-- Setup
------------------------------------------------------------

-- Builds the lockers table from the configured blocks. Idempotent: running it
-- again after a layout change adds the new doors, refreshes the mapping of the
-- ones that moved, and leaves every assignment alone.
function Locker.init()

    local definitions = Config.Lockers()

    if #definitions == 0 then
        return false, "no lockers are configured -- check locker.blocks in smartlocker.json"
    end

    local added, updated, removed, layoutError = LockerDb.ensure_layout(definitions)

    if layoutError then
        -- Nothing else can work: every locker query below would answer from a
        -- table that does not describe the cabinet.
        return false, layoutError
    end

    Logger.info("locker layout: " .. #definitions .. " lockers (" .. added .. " added, " ..
                updated .. " remapped, " .. removed .. " removed)")

    local unwired = 0

    for _, definition in ipairs(definitions) do
        if definition.output_register == nil then
            unwired = unwired + 1
        end
    end

    if unwired > 0 then
        -- Section 6: doors 7 and 8 report their state but have no unlock
        -- output. They are never handed out, and saying so once at start-up is
        -- cheaper than working out later why locker 7 is always free.
        Logger.warning(unwired .. " locker(s) have no unlock output configured and will not be assigned")
    end

    return true

end

function Locker.get(lockerId)
    return LockerDb.get(lockerId)
end

function Locker.all()
    return LockerDb.all()
end

function Locker.get_by_card(cardCode)
    return LockerDb.get_by_card(cardCode)
end

function Locker.summary()
    return LockerDb.summary()
end

------------------------------------------------------------
-- Doors
------------------------------------------------------------

-- Reads one door. Returns true (open), false (closed) or nil when the PLC did
-- not answer -- and nil must never be read as "closed", because that is the
-- reading that would silently swallow a DOOR_OPEN_TIMEOUT.
function Locker.is_open(lockerId)

    local locker = LockerDb.get(lockerId)

    if locker == nil or locker.input_register == nil then
        return nil, "locker " .. tostring(lockerId) .. " has no input mapping"
    end

    local bit, err = Modbus.read_register_bit(locker.input_register, locker.input_bit)

    if bit == nil then
        return nil, err
    end

    -- plc.door_open_value flips the sensor polarity for wiring where a closed
    -- door is the energised one (section 5.1).
    if (Config.plc.door_open_value or 1) == 0 then
        return not bit
    end

    return bit

end

function Locker.is_closed(lockerId)

    local open, err = Locker.is_open(lockerId)

    if open == nil then
        return nil, err
    end

    return not open

end

-- Blocking waits. Tests only -- see the note at the top of this file.
function Locker.wait_open(lockerId, timeoutSeconds)

    local deadline = Time.Deadline(timeoutSeconds or Config.locker.door_open_timeout)

    while not Time.Reached(deadline) do

        Modbus.begin_scan()

        local open = Locker.is_open(lockerId)

        if open == true then
            return true
        end

        Sleep(200)

    end

    return false, "door did not open within " .. tostring(timeoutSeconds or Config.locker.door_open_timeout) .. "s"

end

function Locker.wait_close(lockerId, timeoutSeconds)

    local deadline = Time.Deadline(timeoutSeconds or Config.locker.door_close_timeout)

    while not Time.Reached(deadline) do

        Modbus.begin_scan()

        local open = Locker.is_open(lockerId)

        if open == false then
            return true
        end

        Sleep(200)

    end

    return false, "door did not close within " .. tostring(timeoutSeconds or Config.locker.door_close_timeout) .. "s"

end

------------------------------------------------------------
-- Unlocking
------------------------------------------------------------

-- Fires the unlock output: set, hold lock_trigger_ms, clear (section 5.2). The
-- read-modify-write that preserves the other doors' bits is
-- hardware/modbus.lua's business.
--
-- Returns ok, error. A locker with no output is a configuration fact, not a
-- fault, and says so.
function Locker.unlock(lockerId, cardCode)

    local locker = LockerDb.get(lockerId)

    if locker == nil then
        return false, "no such locker " .. tostring(lockerId)
    end

    if not LockerDb.can_unlock(locker) then
        return false, "locker " .. tostring(locker.locker_number) .. " has no unlock output configured"
    end

    local ok, err = Modbus.trigger_bit(locker.output_register, locker.output_bit,
                                       Config.locker.lock_trigger_ms or 100)

    if not ok then
        Locker.set_error(lockerId, "unlock failed: " .. tostring(err))
        return false, err
    end

    Logger.locker_unlock(locker, cardCode)

    -- From here the door is the state machine's problem.
    runtime[lockerId] = {
        state = Locker.RUNTIME.UNLOCKING,
        card_code = cardCode,
        since = Time.Now(),
        deadline = Time.Deadline(Config.locker.door_open_timeout or 30),
    }

    mirrorRuntime(lockerId, Locker.RUNTIME.UNLOCKING)

    return true

end

function Locker.runtime_state(lockerId)

    local entry = runtime[lockerId]

    return entry and entry.state or Locker.RUNTIME.IDLE

end

function Locker.busy(lockerId)
    return runtime[lockerId] ~= nil
end

function Locker.active_count()

    local count = 0

    for _ in pairs(runtime) do
        count = count + 1
    end

    return count

end

------------------------------------------------------------
-- Error state
------------------------------------------------------------

function Locker.set_error(lockerId, reason)

    local locker = LockerDb.get(lockerId)

    if locker == nil then
        return false
    end

    LockerDb.set_error(lockerId, reason)
    Logger.locker_error(locker, reason, locker.card_code)

    runtime[lockerId] = nil
    mirrorRuntime(lockerId, Locker.RUNTIME.IDLE)

    return true

end

-- Clears ERROR and puts the locker back into the state its assignment implies.
-- An operator action (or a successful open/close cycle) is what should call
-- this; nothing clears an error on its own.
function Locker.clear_error(lockerId)

    local locker = LockerDb.get(lockerId)

    if locker == nil then
        return false
    end

    local status = Locker.STATUS.EMPTY

    if locker.card_code ~= nil then

        status = Locker.STATUS.ASSIGNED

        local employee = EmployeeDb.find_by_card(locker.card_code)

        if employee ~= nil and employee.expire_at ~= nil then
            local expired = Time.IsExpired(employee.expire_at, nil, Config.sync.expire_at_end_of_day)
            if expired then
                status = Locker.STATUS.EXPIRED
            end
        end

    end

    LockerDb.set_status(lockerId, status)
    runtime[lockerId] = nil

    return true, status

end

-- The status an idle locker should be sitting in, given who it belongs to.
-- Used to leave ERROR behind after a clean cycle and by the expiry refresh.
local function restingStatus(locker)

    if locker.card_code == nil then
        return Locker.STATUS.EMPTY
    end

    local employee = EmployeeDb.find_by_card(locker.card_code)

    if employee ~= nil and employee.expire_at ~= nil then

        local expired = Time.IsExpired(employee.expire_at, nil, Config.sync.expire_at_end_of_day)

        if expired then
            return Locker.STATUS.EXPIRED
        end

    end

    return Locker.STATUS.ASSIGNED

end

Locker.resting_status = restingStatus

------------------------------------------------------------
-- The state machine
------------------------------------------------------------

local Buzzer = nil

-- Loaded lazily: hardware/buzzer.lua requires hardware/modbus.lua, which this
-- file also uses, and going through require() at call time keeps the module
-- graph acyclic no matter which of the two is loaded first by a test.
local function buzzer()

    if Buzzer == nil then
        Buzzer = require("hardware.buzzer")
    end

    return Buzzer

end

local function finishCycle(lockerId, locker)

    LockerDb.set_status(lockerId, restingStatus(locker))
    runtime[lockerId] = nil
    mirrorRuntime(lockerId, Locker.RUNTIME.IDLE)

end

local function step(lockerId, entry, open)

    local locker = LockerDb.get(lockerId)

    if locker == nil then
        runtime[lockerId] = nil
        return
    end

    -- A PLC that stopped answering is not evidence that the door stayed shut.
    -- The timers keep running (an unreachable PLC during a swipe still ends in
    -- DOOR_OPEN_TIMEOUT, which is the correct outcome) but no transition is
    -- taken on a reading we do not have.
    if open == nil then

        if Time.Reached(entry.deadline) then

            local event = (entry.state == Locker.RUNTIME.UNLOCKING)
                and Logger.EVENTS.DOOR_OPEN_TIMEOUT
                or Logger.EVENTS.DOOR_CLOSE_TIMEOUT

            Logger.door_timeout(event, locker, entry.card_code, Time.Since(entry.since))
            buzzer().door_error()
            Locker.set_error(lockerId, event .. " (no reply from the PLC)")

        end

        return

    end

    if entry.state == Locker.RUNTIME.UNLOCKING then

        if open then

            LockerDb.mark_open(lockerId, Time.Stamp())
            Logger.door_open(locker, entry.card_code, Time.Since(entry.since))

            entry.state = Locker.RUNTIME.WAIT_CLOSE
            entry.opened_at = Time.Now()
            entry.deadline = Time.Deadline(Config.locker.door_close_timeout or 30)

            mirrorRuntime(lockerId, Locker.RUNTIME.WAIT_CLOSE)

        elseif Time.Reached(entry.deadline) then

            -- Section 11: the door never opened.
            Logger.door_timeout(Logger.EVENTS.DOOR_OPEN_TIMEOUT, locker, entry.card_code, Time.Since(entry.since))
            buzzer().door_error()
            Locker.set_error(lockerId, "DOOR_OPEN_TIMEOUT")

        end

        return

    end

    if entry.state == Locker.RUNTIME.WAIT_CLOSE then

        if not open then

            LockerDb.mark_closed(lockerId, Time.Stamp())
            Logger.door_close(locker, entry.card_code, Time.Since(entry.opened_at or entry.since))

            finishCycle(lockerId, locker)

        elseif Time.Reached(entry.deadline) then

            -- Section 11: left standing open.
            Logger.door_timeout(Logger.EVENTS.DOOR_CLOSE_TIMEOUT, locker, entry.card_code,
                                Time.Since(entry.opened_at or entry.since))
            buzzer().door_error()
            Locker.set_error(lockerId, "DOOR_CLOSE_TIMEOUT")

        end

    end

end

-- A door that finally closed after its close timeout has fixed the thing that
-- was wrong with it. Section 11 puts a locker into ERROR when it is left
-- standing open; once it is shut there is nothing wrong any more, and leaving
-- it red would mean a fault on the dashboard for a cabinet that is fine and an
-- operator clearing it by hand for no reason.
--
-- ONLY a close timeout is cleared this way. An ERROR from a failed unlock, or
-- from a PLC that stopped answering, has nothing to do with the door being
-- shut -- clearing those here would hide a fault that is still present. The
-- reason string is what tells them apart, which is why set_error stores one.
local function recoverAfterClose(lockerId, locker)

    if locker.status ~= Locker.STATUS.ERROR then
        return false
    end

    local reason = tostring(locker.last_error or "")

    if not reason:find(Logger.EVENTS.DOOR_CLOSE_TIMEOUT, 1, true) then
        return false
    end

    Logger.info("locker " .. tostring(locker.locker_number) .. " closed after " ..
                Logger.EVENTS.DOOR_CLOSE_TIMEOUT .. "; clearing the error")

    local cleared, status = Locker.clear_error(lockerId)

    if cleared then
        Logger.event(Logger.TYPES.DOOR, {
            event = Logger.EVENTS.DOOR_CLOSE,
            locker_id = lockerId,
            locker_number = locker.locker_number,
            block_id = locker.block_id,
            card_code = locker.card_code,
        })

        Logger.system("LOCKER_RECOVERED",
                      "locker " .. tostring(locker.locker_number) ..
                      " was closed after a close timeout; back to " .. tostring(status),
                      locker.last_error)
    end

    return cleared

end

-- One pass of the machine. Called from the main loop; Modbus.begin_scan() must
-- have been called first, which is what collapses the per-locker reads into one
-- read per register.
--
-- Every locker is read, not just the busy ones: a door opened without a swipe
-- (forced, or left ajar) is exactly the kind of thing this system exists to
-- record.
function Locker.poll()

    local transitions = 0

    for _, locker in ipairs(LockerDb.all()) do

        local lockerId = locker.id
        local open = nil

        if locker.input_register ~= nil then
            open = Locker.is_open(lockerId)
        end

        local previous = doorState[lockerId]

        -- The dashboard's "Tủ mở" badge. Written on the edge only, whether or
        -- not this door is in the middle of a swipe.
        if open ~= nil and open ~= previous then
            mirrorDoor(lockerId, open)
        end

        if open ~= nil and previous ~= nil and open ~= previous and runtime[lockerId] == nil then

            -- Movement on a door nobody unlocked.
            if Config.locker.report_unexpected_open ~= false then

                if open then
                    Logger.door(Logger.EVENTS.DOOR_OPEN, locker, locker.card_code, nil, "WARNING")
                    Logger.warning("locker " .. tostring(locker.locker_number) ..
                                   " opened without an unlock")
                else
                    Logger.door(Logger.EVENTS.DOOR_CLOSE, locker, locker.card_code)
                end

            end

            LockerDb.update(lockerId, open and { last_open_at = Time.Stamp() } or { last_close_at = Time.Stamp() })

        end

        -- A door that is SHUT and has no cycle in flight: if it is still marked
        -- ERROR for having been left open, that is no longer true and the
        -- locker can go back into service.
        --
        -- Checked as a LEVEL, not on the closing edge. An edge is the obvious
        -- place for it and it is not enough: the door can close while the
        -- gateway is down, or while the PLC is unreachable and reconcile() is
        -- skipped -- and then no transition ever arrives to notice, leaving the
        -- locker red forever. This is a no-op unless the status and the reason
        -- both match, so running it every pass costs two field reads.
        if open == false and runtime[lockerId] == nil then
            recoverAfterClose(lockerId, locker)
        end

        if open ~= nil then
            doorState[lockerId] = open
        end

        local entry = runtime[lockerId]

        if entry ~= nil then
            local before = entry.state
            step(lockerId, entry, open)
            if runtime[lockerId] == nil or runtime[lockerId].state ~= before then
                transitions = transitions + 1
            end
        end

    end

    return transitions

end

------------------------------------------------------------
-- Restart recovery (Plan section 37)
------------------------------------------------------------

-- Reads every door and makes the stored state agree with the hardware. A door
-- found standing open is put into WAIT_CLOSE with a fresh timer rather than
-- being ignored: someone was in the middle of using it when the gateway
-- restarted, and if it is still open in 30 seconds that is a genuine
-- DOOR_CLOSE_TIMEOUT.
function Locker.reconcile()

    Modbus.begin_scan()

    local report = { total = 0, open = 0, unreadable = 0, errors = 0 }

    for _, locker in ipairs(LockerDb.all()) do

        report.total = report.total + 1

        local open = nil

        if locker.input_register ~= nil then
            open = Locker.is_open(locker.id)
        end

        if open == nil then

            report.unreadable = report.unreadable + 1

        else

            doorState[locker.id] = open
            mirrorDoor(locker.id, open)

            if open then

                report.open = report.open + 1

                runtime[locker.id] = {
                    state = Locker.RUNTIME.WAIT_CLOSE,
                    card_code = locker.card_code,
                    since = Time.Now(),
                    opened_at = Time.Now(),
                    deadline = Time.Deadline(Config.locker.door_close_timeout or 30),
                }

                mirrorRuntime(locker.id, Locker.RUNTIME.WAIT_CLOSE)

            else
                -- A door that is shut and has no cycle in flight: whatever the
                -- previous run left in runtime_state is stale by definition.
                mirrorRuntime(locker.id, Locker.RUNTIME.IDLE)

                -- Shut, but recorded as ERROR for having been left open. The
                -- closing happened while this gateway was down, so no edge will
                -- ever arrive to notice it -- without this the locker would
                -- stay red until somebody cleared it by hand.
                recoverAfterClose(locker.id, locker)
            end

        end

        if locker.status == Locker.STATUS.ERROR then
            report.errors = report.errors + 1
        end

    end

    Logger.system("RESTART_RECOVERY",
                  "door states read from the PLC: " .. report.open .. " open, " ..
                  report.unreadable .. " unreadable, " .. report.errors .. " in ERROR, of " ..
                  report.total .. " lockers",
                  nil,
                  report.unreadable > 0 and "WARNING" or "INFO")

    return report

end

-- Recomputes EXPIRED / ASSIGNED for every occupied locker. Called after each
-- synchronisation and once a day, so a contractor whose card lapsed overnight
-- shows yellow before anybody swipes it (Plan section 10).
function Locker.refresh_expiry()

    local changed = 0

    for _, locker in ipairs(LockerDb.all()) do

        if locker.status ~= Locker.STATUS.ERROR and locker.card_code ~= nil then

            local wanted = restingStatus(locker)

            if locker.status ~= wanted then
                LockerDb.set_status(locker.id, wanted)
                changed = changed + 1
            end

        end

    end

    return changed

end

------------------------------------------------------------
-- Frontend view (Plan section 13)
------------------------------------------------------------

function Locker.snapshot()

    local out = {}

    for _, locker in ipairs(LockerDb.all()) do

        local employee = locker.card_code and EmployeeDb.find_by_card(locker.card_code) or nil

        out[#out + 1] = {
            locker_id = locker.id,
            block_id = locker.block_id,
            block_name = locker.block_name,
            locker_number = locker.locker_number,
            locker_type = locker.locker_type,
            status = locker.status,
            runtime_state = Locker.runtime_state(locker.id),
            door = doorState[locker.id],
            card_code = locker.card_code,
            username = employee and employee.username or nil,
            gender = employee and employee.gender or nil,
            assigned_at = locker.assigned_at,
            expire_at = locker.expire_at,
            last_open_at = locker.last_open_at,
            last_close_at = locker.last_close_at,
            last_error = locker.last_error,
            can_unlock = LockerDb.can_unlock(locker),
        }

    end

    return out

end

function Locker.door_states()
    return doorState
end

-- Test support.
function Locker.reset_runtime()
    runtime = {}
    doorState = {}
end

return Locker
