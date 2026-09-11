-- core/assignment.lua
-- Who gets which locker (Plan sections 7 and 29).
--
-- The rules, in the order they are applied:
--   1. the locker type must match the person's role -- an employee never gets a
--      a contractor locker unless locker.allow_cross_type says otherwise
--   2. the locker must be free, and must have an unlock output wired
--   3. lowest number first, for both genders; with locker.female_priority.enabled
--      a woman is offered the reserved range first and only spills outside it
--      once that range is full
--   4. no locker available is an error to log, never a silent no-op
--
-- Every assignment and release runs inside one database transaction, because
-- the relationship is stored on both sides (employees.locker_id and
-- lockers.card_code). Half of it committed is a locker that belongs to nobody
-- while its owner believes otherwise -- which is exactly the inconsistency Plan
-- section 15 asks transactions to prevent.

local Config          = require("config")
local ContractorUsage = require("core.contractor_usage")
local Database        = require("database.database")
local Employee        = require("core.employee")
local EmployeeDb      = require("database.employee_db")
-- For the reclaim sweep only, to skip a door that is mid-cycle. Neither
-- core/locker nor core/contractor_usage requires this file back, so this is a
-- diamond and not a cycle.
local Locker          = require("core.locker")
local LockerDb        = require("database.locker_db")
local Logger          = require("utils.logger")
local Time            = require("utils.time")

local Assignment = {}

------------------------------------------------------------
-- Type matching
------------------------------------------------------------

-- A role maps onto the locker type of the same name. Blocks are configured with
-- `type = "employee"` / `"contractor"`, so this is deliberately trivial -- the
-- mapping exists so a site with differently named block types has one line to
-- change rather than a search through the business logic.
function Assignment.locker_type_for(employee)

    if employee == nil then
        return nil
    end

    return employee.role or Employee.ROLE.EMPLOYEE

end

-- Whether a person may use a locker of this type at all (Plan section 9).
function Assignment.type_matches(employee, locker)

    if employee == nil or locker == nil then
        return false
    end

    if Config.locker.allow_cross_type then
        return true
    end

    return locker.locker_type == Assignment.locker_type_for(employee)

end

------------------------------------------------------------
-- Choosing a locker
------------------------------------------------------------

-- Plan section 29. Returns the locker row, or nil plus the reason.
function Assignment.find_available(lockerType, gender, blockId)

    local candidates = LockerDb.available(lockerType, blockId)

    if #candidates == 0 then
        return nil, "no free " .. tostring(lockerType) .. " locker"
    end

    local priority = Config.locker.female_priority or {}

    if priority.enabled and gender == Employee.GENDER.FEMALE then

        local first = priority.start or 1
        local last = priority["end"] or first

        for _, locker in ipairs(candidates) do
            if locker.locker_number >= first and locker.locker_number <= last then
                return locker
            end
        end

        -- The reserved range is full. Falling through to the general pool is
        -- deliberate: a woman with no locker at all would be a worse outcome
        -- than one outside the preferred range.
        Logger.info("female priority range " .. first .. "-" .. last ..
                    " is full; assigning from the general pool")

    end

    -- LockerDb.available() already sorts by block then number, so the head of
    -- the list is the lowest available locker -- the rule both genders share.
    return candidates[1]

end

------------------------------------------------------------
-- Assign / release
------------------------------------------------------------

-- Assignment.assign(employee [, options]) -> locker, error
--
-- Idempotent: an employee who already holds a valid locker keeps it, and gets
-- it returned. That matters because the daily synchronisation runs this over
-- every active card.
function Assignment.assign(employee, options)

    options = options or {}

    if employee == nil then
        return nil, "no employee record"
    end

    if employee.active == false and not options.force then
        return nil, "card " .. tostring(employee.card_code) .. " is inactive"
    end

    -- Already holds one?
    local existing = LockerDb.get_by_card(employee.card_code)

    if existing ~= nil then

        if Assignment.type_matches(employee, existing) then
            return existing
        end

        -- Their role changed under them (an employee became a contractor).
        -- The old locker is released and a correct one is picked below;
        -- leaving them in the wrong cabinet would fail every swipe with
        -- WRONG_LOCKER_TYPE and look like a hardware fault.
        Logger.info("card " .. tostring(employee.card_code) .. " holds a " ..
                    tostring(existing.locker_type) .. " locker but is now " ..
                    tostring(employee.role) .. "; reassigning")

        Assignment.release_locker(existing.id, "role changed")

    end

    local lockerType = Assignment.locker_type_for(employee)
    local locker, reason = Assignment.find_available(lockerType, employee.gender, options.block_id)

    if locker == nil then

        -- Section 7: do not assign, write an error log.
        Logger.assign_failed(employee, reason)
        return nil, reason

    end

    local timestamp = Time.Stamp()

    local ok, err = Database.transaction_do(function()

        LockerDb.assign(locker.id, employee, timestamp)
        EmployeeDb.set_locker(employee.id, locker.id)

        return true

    end)

    if not ok then
        Logger.assign_failed(employee, "database error: " .. tostring(err))
        return nil, err
    end

    local stored = LockerDb.get(locker.id)

    Logger.locker_assigned(employee, stored)

    return stored

end

-- Frees a locker and detaches whoever was in it. Safe to call on an already
-- empty locker.
function Assignment.release_locker(lockerId, reason)

    local locker = LockerDb.get(lockerId)

    if locker == nil then
        return false, "no such locker " .. tostring(lockerId)
    end

    local cardCode = locker.card_code

    if cardCode == nil then
        return true
    end

    local employee = EmployeeDb.find_by_card(cardCode)

    local ok, err = Database.transaction_do(function()

        LockerDb.release(lockerId)

        if employee ~= nil then
            EmployeeDb.set_locker(employee.id, nil)
        end

        return true

    end)

    if not ok then
        return false, err
    end

    Logger.locker_released(locker, reason, cardCode)

    return true

end

-- Which holding model this site runs (locker.contractor_mode).
function Assignment.contractor_mode()

    local mode = math.tointeger(tonumber(Config.locker.contractor_mode)) or 1

    if mode ~= 2 then
        return 1
    end

    return 2

end

local function holdSeconds()
    return math.max(0, tonumber(Config.locker.hold_hours) or 24) * 3600
end

-- Assignment.publish_policy()
--
-- Writes the holding model into the database's `meta` table so the WEB LAYER
-- can read it. The floor plan draws a countdown, and to do that it has to know
-- which deadline applies -- but the policy lives in smartlocker.json, which is
-- this script's configuration and not the gateway's, so the C++ side cannot see
-- it any other way.
--
-- The database is already the channel between the two (the UI reads it
-- read-only), so this adds no new coupling: it puts three numbers where the
-- reader already looks. Rewritten on every start, because the file may have
-- changed while the gateway was down.
function Assignment.publish_policy()

    Database.meta_set("contractor_mode", Assignment.contractor_mode())
    Database.meta_set("hold_hours", math.max(0, tonumber(Config.locker.hold_hours) or 24))
    Database.meta_set("auto_reclaim", Config.locker.auto_reclaim ~= false and 1 or 0)

    return true

end

-- Assignment.hold_expired(locker [, now]) -> boolean, reason
--
-- Mode 2 only. The locker has been with this person for longer than
-- hold_hours, counted from when it was handed to them.
--
-- UNLIKE THE MODE 1 RULE, THIS DOES NOT ASK WHETHER THEY FINISHED. That is the
-- whole difference between the two models, and it is not an oversight: a
-- hot-desk cabinet promises the door to the next person at a known time, and a
-- promise that only holds when the previous occupant remembered to sign out is
-- not one worth making. A site that would rather wait for the person is asking
-- for mode 1.
function Assignment.hold_expired(locker, now)

    if Assignment.contractor_mode() ~= 2 then
        return false, "not in hold mode"
    end

    if locker == nil or locker.card_code == nil then
        return false, "the locker is not assigned"
    end

    -- Scoped to the contractor block, because that is the block the mode is
    -- about. An employee cabinet on the same gateway keeps its own rules.
    if locker.locker_type ~= Employee.ROLE.CONTRACTOR then
        return false, "not a contractor locker"
    end

    local assignedAt = Time.Parse(locker.assigned_at)

    if assignedAt == nil then
        -- No usable timestamp: refuse rather than guess. A locker whose
        -- assigned_at cannot be read would otherwise be reclaimed on every
        -- single sweep, which is the worst possible reading of a missing value.
        return false, "assigned_at is missing or unreadable"
    end

    local held = (now or Time.Now()) - assignedAt
    local limit = holdSeconds()

    if limit == 0 or held < limit then
        return false, "held for " .. math.floor(held / 60) .. " min of " ..
                      math.floor(limit / 60) .. " min"
    end

    return true, "held since " .. tostring(locker.assigned_at) .. " (" ..
                 math.floor(held / 3600) .. "h, limit " .. (limit / 3600) .. "h)"

end

-- Every locker whose hold has run out. Mode 2 only; an empty list in mode 1.
function Assignment.expired_holds(now)

    local out = {}

    if Assignment.contractor_mode() ~= 2 then
        return out
    end

    now = now or Time.Now()

    for _, locker in ipairs(LockerDb.all()) do

        local over, reason = Assignment.hold_expired(locker, now)

        if over then
            out[#out + 1] = { locker = locker, reason = reason }
        end

    end

    return out

end

-- Takes one locker back, or says why it could not. Shared by both rules so
-- they cannot drift apart on the details that matter -- skipping a door that is
-- mid-cycle, and recording WHY in the released event.
local function reclaimOne(locker, reason, counters)

    if locker == nil then
        counters.skipped = counters.skipped + 1
        return
    end

    if Locker.busy(locker.id) then
        -- It will still be reclaimable on the next sweep. Releasing a locker
        -- while its own state machine is opening it would have the two
        -- disagree about who it belongs to.
        Logger.info("reclaim: locker " .. tostring(locker.locker_number) .. " is " ..
                    Locker.runtime_state(locker.id) .. "; leaving it for the next sweep")
        counters.skipped = counters.skipped + 1
        return
    end

    local ok, err = Assignment.release_locker(locker.id, "auto-reclaimed -- " .. reason)

    if ok then
        counters.released = counters.released + 1
    else
        counters.skipped = counters.skipped + 1
        Logger.warning("reclaim: could not release locker " .. tostring(locker.locker_number) ..
                       ": " .. tostring(err))
    end

end

-- Assignment.reclaim() -> released, skipped
--
-- Runs whichever rules the configured mode calls for:
--
--   mode 1   finished their last day AND period ended
--   mode 2   the above, PLUS any locker held longer than hold_hours
--
-- WHAT MODE 1 DELIBERATELY LEAVES ALONE: a locker whose holder's period ended
-- while the day was still open. Nobody has told this system that locker is
-- empty, and a scheduled job is the worst possible thing to be holding
-- somebody's bag when it decides otherwise. Those stay EXPIRED for a person to
-- deal with through the admin override. Mode 2 makes the opposite trade
-- knowingly -- see Assignment.hold_expired.
function Assignment.reclaim()

    if Config.locker.auto_reclaim == false then
        return 0, 0
    end

    local counters = { released = 0, skipped = 0 }
    local seen = {}

    -- Finished and out of period. Applies in both modes.
    for _, entry in ipairs(ContractorUsage.reclaimable()) do

        local locker = LockerDb.get_by_card(entry.employee.card_code)

        if locker ~= nil then
            seen[locker.id] = true
        end

        reclaimOne(locker, entry.reason, counters)

    end

    -- Held too long. Mode 2 only, and skipping anything the rule above already
    -- took so one locker cannot be counted (or logged) twice.
    for _, entry in ipairs(Assignment.expired_holds()) do

        if not seen[entry.locker.id] then
            seen[entry.locker.id] = true
            reclaimOne(entry.locker, entry.reason, counters)
        end

    end

    if counters.released > 0 then
        Logger.system("LOCKERS_RECLAIMED",
                      "reclaimed " .. counters.released .. " locker(s) (mode " ..
                      Assignment.contractor_mode() .. ")",
                      counters.skipped > 0 and (tostring(counters.skipped) .. " skipped") or nil)
    end

    return counters.released, counters.skipped

end

function Assignment.release(cardCode, reason)

    local locker = LockerDb.get_by_card(cardCode)

    if locker == nil then

        -- No locker, but the employee row may still claim one. Clear it so the
        -- two sides agree.
        local employee = EmployeeDb.find_by_card(cardCode)

        if employee ~= nil and employee.locker_id ~= nil then
            EmployeeDb.set_locker(employee.id, nil)
        end

        return true

    end

    return Assignment.release_locker(locker.id, reason)

end

------------------------------------------------------------
-- Synchronisation hooks
------------------------------------------------------------

-- Applies the locker side of an employee synchronisation (Plan section 3.1's
-- "Update locker assignments" step). `changes` is what Employee.sync returned.
function Assignment.apply_changes(changes, options)

    options = options or {}

    local stats = { assigned = 0, released = 0, failed = 0 }

    local function assignIfWanted(employee)

        if employee.active == false then
            return
        end

        -- An expired contractor keeps whatever locker they have (so their
        -- belongings are not handed to somebody else) but is never given a new
        -- one.
        if Employee.is_expired(employee) then
            return
        end

        if LockerDb.get_by_card(employee.card_code) ~= nil then
            return
        end

        local locker = Assignment.assign(employee)

        if locker ~= nil then
            stats.assigned = stats.assigned + 1
        else
            stats.failed = stats.failed + 1
        end

    end

    if Config.sync.assign_on_sync ~= false then

        for _, employee in ipairs(changes.added or {}) do
            assignIfWanted(employee)
        end

        for _, employee in ipairs(changes.updated or {}) do
            assignIfWanted(employee)
        end

    end

    -- Cards the server withdrew.
    for _, employee in ipairs(changes.deactivated or {}) do

        if (options.on_removed or Config.sync.on_removed) == "release" then

            if Assignment.release(employee.card_code, "card removed from the server list") then
                stats.released = stats.released + 1
            end

        end

    end

    return stats

end

------------------------------------------------------------
-- Consistency
------------------------------------------------------------

-- Repairs the two-sided relationship. Runs at start-up and after every sync,
-- because a database restored by hand, an interrupted write, or a locker block
-- that shrank in the configuration can all leave one side pointing at nothing.
--
-- Returns a report of what it fixed -- silence would make a recurring
-- corruption invisible.
function Assignment.reconcile()

    local report = { orphan_lockers = 0, orphan_employees = 0, mismatched = 0, wrong_type = 0 }

    -- 1. Lockers holding a card that no employee owns any more.
    for _, locker in ipairs(LockerDb.all()) do

        if locker.card_code ~= nil then

            local employee = EmployeeDb.find_by_card(locker.card_code)

            if employee == nil then

                report.orphan_lockers = report.orphan_lockers + 1
                Logger.warning("locker " .. tostring(locker.locker_number) .. " holds unknown card " ..
                               tostring(locker.card_code) .. "; releasing")
                Assignment.release_locker(locker.id, "no employee record for this card")

            elseif employee.locker_id ~= locker.id then

                -- The employee row lost track of it. The locker is the
                -- authority here: it is the physical door with the person's
                -- belongings in it.
                report.mismatched = report.mismatched + 1
                EmployeeDb.set_locker(employee.id, locker.id)

            elseif not Assignment.type_matches(employee, locker) then

                report.wrong_type = report.wrong_type + 1
                Logger.warning("locker " .. tostring(locker.locker_number) .. " (" ..
                               tostring(locker.locker_type) .. ") is held by a " ..
                               tostring(employee.role) .. "; leaving it in place -- " ..
                               "release it manually once the person has emptied it")

            end

        end

    end

    -- 2. Employees pointing at a locker that no longer exists or belongs to
    --    somebody else.
    for _, employee in ipairs(EmployeeDb.all()) do

        if employee.locker_id ~= nil then

            local locker = LockerDb.get(employee.locker_id)

            if locker == nil or locker.card_code == nil
               or EmployeeDb.normalize_code(locker.card_code) ~= EmployeeDb.normalize_code(employee.card_code) then

                report.orphan_employees = report.orphan_employees + 1
                EmployeeDb.update(employee.id, { locker_id = Database.NULL })

            end

        end

    end

    local total = report.orphan_lockers + report.orphan_employees + report.mismatched + report.wrong_type

    if total > 0 then
        Logger.system("ASSIGNMENT_RECONCILED",
                      "assignment consistency: " .. report.orphan_lockers .. " orphan locker(s), " ..
                      report.orphan_employees .. " stale employee link(s), " ..
                      report.mismatched .. " relinked, " .. report.wrong_type .. " wrong type",
                      nil, "WARNING")
    end

    return report

end

return Assignment
