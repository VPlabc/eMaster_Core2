-- core/contractor_usage.lua
-- What a contractor's card is allowed to do TODAY (CardScanPlan section 1).
--
-- WHY THIS EXISTS. A contractor's expire_at answers one question -- has the
-- registered period ended -- and the site needs a second one answered: has this
-- card been used today, and where in the day is it. The plan asks for both, and
-- keeping them apart is the whole design:
--
--   THE PERIOD    start_at .. expire_at, sent by the server. It decides whether
--                 the card works at all. Nothing in this file ever extends it:
--                 "khong tu dong gia han chi vi contractor chua su dung locker"
--                 -- a contractor who never opened their locker does not earn
--                 extra days for it.
--
--   THE DAY       usage_date plus usage_status, kept by this file. It is a
--                 record of the shift: NOT_STARTED before the first open, USING
--                 after the morning one, COMPLETED after the evening one, and
--                 EXPIRED once the day it describes is over. It resets with the
--                 date; yesterday's EXPIRED day never keeps anybody out today.
--
-- So there are two EXPIREDs in this application and they are not the same
-- thing. The one that refuses a card is Employee.is_expired (the period). The
-- one stored in usage_status is a finished day, and is what the dashboard shows
-- as the contractor's state.
--
-- FINISHING THE DAY IS A GESTURE, NOT A TIME. Every open is USING, however
-- many there are: a contractor who opens their locker at 08:10, at noon and
-- again at 16:00 has used it three times and finished nothing. The day ends
-- when they say it ends -- complete_scan_count scans of the same card in a row,
-- close together (complete_scan_timeout). That is the only thing that writes
-- COMPLETED.
--
-- The distinction the timeout draws is the whole point: three opens spread
-- across a shift are three opens; three scans inside ten seconds are a
-- decision. Nothing about the hour of the day can tell those apart, which is
-- why the afternoon window no longer completes anything -- it only gates
-- access, and only when usage_timeout_enabled is on.
--
-- The state machine:
--
--     issued -> NOT_STARTED
--       any open                        -> USING
--       N scans in a row                -> COMPLETED
--       an open while COMPLETED         -> USING again (they came back, and
--                                          their period has not ended)
--       N scans in a row again          -> COMPLETED again
--       the day ends while still USING  -> EXPIRED (that day only)
--       the next day, within the period -> NOT_STARTED again
--
-- Nothing here touches a locker or a door. It records what happened and answers
-- yes/no; core/access.lua does the opening.

local Config     = require("config")
local Database   = require("database.database")
local Employee   = require("core.employee")
local EmployeeDb = require("database.employee_db")
local Logger     = require("utils.logger")
local Time       = require("utils.time")

local ContractorUsage = {}

ContractorUsage.STATUS = {
    NOT_STARTED = "NOT_STARTED",
    USING       = "USING",
    COMPLETED   = "COMPLETED",
    EXPIRED     = "EXPIRED",
}

-- Where in the working day a moment falls. Reported by ContractorUsage.window()
-- and used both to classify an open and, when the hard gate is on, to refuse
-- one.
ContractorUsage.WINDOW = {
    BEFORE    = "before",       -- earlier than morning_start
    MORNING   = "morning",      -- morning_start .. morning_end
    MIDDAY    = "midday",       -- between the two windows
    AFTERNOON = "afternoon",    -- afternoon_start .. afternoon_end
    AFTER     = "after",        -- past afternoon_end (overtime)
}

-- Where a server-pushed overtime end-time is kept. In the database rather than
-- in smartlocker.json: it is a fact about tonight, and a configuration file
-- would carry it into every night after this one.
local OVERTIME_KEY = "contractor_overtime_end"
local OVERTIME_DATE_KEY = "contractor_overtime_date"

local stats = {
    started = 0,
    completed = 0,
    reopened = 0,          -- COMPLETED -> USING, the "came back for it" case
    expired = 0,
    denied_not_started = 0,
    denied_out_of_hours = 0,
}

-- The finishing gesture, tracked one slot at a time rather than per card: one
-- reader serves one person at a time, so somebody else presenting a card in the
-- middle of a sequence has broken it -- exactly as it does for the admin
-- sequence in core/admin_access.lua.
local sequence = { card = nil, count = 0, at = nil }

------------------------------------------------------------
-- Settings
------------------------------------------------------------

local function settings()
    return Config.contractor or {}
end

-- "08:00" -> 480. Accepts "8:00" and "0800" too, because a configuration file
-- edited by hand will eventually contain both. An unreadable value returns nil
-- and the caller falls back to its default, having said so.
local function minutesOf(text)

    if text == nil then
        return nil
    end

    if type(text) == "number" then
        return math.floor(text)
    end

    local value = tostring(text)

    local hour, minute = value:match("^%s*(%d%d?):(%d%d)%s*$")

    if hour == nil then
        hour, minute = value:match("^%s*(%d%d)(%d%d)%s*$")
    end

    if hour == nil then
        return nil
    end

    hour, minute = tonumber(hour), tonumber(minute)

    if hour > 23 or minute > 59 then
        return nil
    end

    return hour * 60 + minute

end

ContractorUsage.minutes_of = minutesOf

-- HH:MM again, for messages and for the frontend.
local function clockOf(minutes)

    if minutes == nil then
        return nil
    end

    return string.format("%02d:%02d", math.floor(minutes / 60), minutes % 60)

end

ContractorUsage.clock_of = clockOf

local complained = {}

local function boundary(key, fallback)

    local raw = settings()[key]
    local value = minutesOf(raw)

    if value == nil then

        if raw ~= nil and not complained[key] then
            complained[key] = true
            Logger.warning("contractor." .. key .. " = '" .. tostring(raw) ..
                           "' is not a HH:MM time; using the default " .. fallback)
        end

        return minutesOf(fallback)

    end

    return value

end

------------------------------------------------------------
-- Overtime (CardScanPlan section 2)
------------------------------------------------------------

-- ContractorUsage.set_overtime("21:00" [, when]) -> ok, error
--
-- The server extending tonight's shift. Stored against the DATE it applies to,
-- so a gateway restarted at 20:00 still knows the shift runs to 21:00 and a
-- gateway still running tomorrow morning does not.
function ContractorUsage.set_overtime(value, when)

    if not Database.is_open() then
        return false, "the database is not open"
    end

    if value == nil or value == "" then

        Database.meta_set(OVERTIME_KEY, nil)
        Database.meta_set(OVERTIME_DATE_KEY, nil)

        Logger.info("contractor overtime cleared")

        Logger.event(Logger.TYPES.ACCESS, {
            event = Logger.EVENTS.OVERTIME_UPDATED,
            result = "CLEARED",
            reason = "the afternoon window is back to its configured end",
        })

        return true

    end

    local minutes = minutesOf(value)

    if minutes == nil then
        return false, "'" .. tostring(value) .. "' is not a HH:MM time"
    end

    local normalized = clockOf(minutes)

    Database.meta_set(OVERTIME_KEY, normalized)
    Database.meta_set(OVERTIME_DATE_KEY, Time.Date(when))

    Logger.info("contractor overtime: the afternoon window now ends at " .. normalized)

    Logger.event(Logger.TYPES.ACCESS, {
        event = Logger.EVENTS.OVERTIME_UPDATED,
        result = "SET",
        reason = "afternoon_end extended to " .. normalized,
    })

    return true

end

-- The overtime currently in force, or nil. Expires with the day unless
-- contractor.overtime_expires_daily is turned off.
function ContractorUsage.overtime(when)

    if not Database.is_open() then
        return nil
    end

    local stored = Database.meta_get(OVERTIME_KEY)

    if stored == nil then
        return nil
    end

    if settings().overtime_expires_daily == false then
        return stored
    end

    if Database.meta_get(OVERTIME_DATE_KEY) ~= Time.Date(when) then
        return nil
    end

    return stored

end

------------------------------------------------------------
-- The working day
------------------------------------------------------------

-- The four boundaries, in minutes since midnight, with the overtime extension
-- already applied. afternoon_end is never allowed to fall before
-- afternoon_start: an overtime value of "02:00" (meaning two in the morning)
-- would otherwise make the afternoon window empty and refuse everybody.
function ContractorUsage.boundaries(when)

    local bounds = {
        morning_start   = boundary("morning_start", "08:00"),
        morning_end     = boundary("morning_end", "09:30"),
        afternoon_start = boundary("afternoon_start", "17:00"),
        afternoon_end   = boundary("afternoon_end", "19:00"),
    }

    bounds.overtime = ContractorUsage.overtime(when)

    local extended = minutesOf(bounds.overtime)

    if extended ~= nil and extended > bounds.afternoon_end then
        bounds.afternoon_end = extended
    end

    if bounds.afternoon_end < bounds.afternoon_start then
        bounds.afternoon_end = bounds.afternoon_start
    end

    return bounds

end

-- ContractorUsage.window([when]) -> window, boundaries
function ContractorUsage.window(when)

    local bounds = ContractorUsage.boundaries(when)
    local clock = os.date("*t", when or Time.Now())
    local minutes = clock.hour * 60 + clock.min

    local window

    if minutes < bounds.morning_start then
        window = ContractorUsage.WINDOW.BEFORE
    elseif minutes <= bounds.morning_end then
        window = ContractorUsage.WINDOW.MORNING
    elseif minutes < bounds.afternoon_start then
        window = ContractorUsage.WINDOW.MIDDAY
    elseif minutes <= bounds.afternoon_end then
        window = ContractorUsage.WINDOW.AFTERNOON
    else
        window = ContractorUsage.WINDOW.AFTER
    end

    return window, bounds

end

------------------------------------------------------------
-- The finishing gesture
------------------------------------------------------------

local function completeTarget()
    return math.max(0, math.tointeger(tonumber(settings().complete_scan_count)) or 3)
end

-- 0 means "no limit": every open of the day counts towards the gesture, which
-- turns it into "any N opens today finish the day".
local function completeTimeout()
    return math.max(0, tonumber(settings().complete_scan_timeout) or 10)
end

-- ContractorUsage.note_scan(cardCode, now) -> count, finishes
--
-- Counts one presentation of a contractor's card and says whether it is the one
-- that finishes the day. Called by on_locker_open; exposed so a test (or a
-- diagnostic page) can drive the sequence without opening a door.
function ContractorUsage.note_scan(cardCode, now)

    now = now or Time.Now()

    local timeout = completeTimeout()
    local continues = sequence.card == cardCode
        and sequence.at ~= nil
        and (timeout == 0 or (now - sequence.at) <= timeout)

    if continues then
        sequence.count = sequence.count + 1
    else
        sequence.card = cardCode
        sequence.count = 1
    end

    sequence.at = now

    local target = completeTarget()

    if target > 0 and sequence.count >= target then
        -- Consumed. The next scan starts a fresh sequence, which is what makes
        -- "finish, come back, finish again" work.
        ContractorUsage.reset_sequence()
        return target, true
    end

    return sequence.count, false

end

function ContractorUsage.reset_sequence()
    sequence = { card = nil, count = 0, at = nil }
end

-- For the dashboard and the tests: how far into the gesture this card is.
function ContractorUsage.scan_state()

    return {
        card_code = sequence.card,
        count = sequence.count,
        required = completeTarget(),
        timeout = completeTimeout(),
        at = sequence.at and Time.Stamp(sequence.at) or nil,
    }

end

------------------------------------------------------------
-- Reading a record
------------------------------------------------------------

function ContractorUsage.is_contractor(employee)
    return employee ~= nil and employee.role == Employee.ROLE.CONTRACTOR
end

-- The card's registered period is over. This -- not usage_status -- is what
-- refuses a card (section 1: after EXPIRED, do not unlock, long beep).
function ContractorUsage.is_expired(employee, now)
    return Employee.is_expired(employee, now)
end

-- Has the registered period begun? A card with no start_at has, which is the
-- normal case: most servers send only an expiry.
function ContractorUsage.has_started(employee, now)

    if employee == nil or employee.start_at == nil or employee.start_at == "" then
        return true
    end

    local epoch = Time.Parse(employee.start_at)

    if epoch == nil then
        -- An unreadable start date is treated as "already started", the
        -- opposite of the unreadable-expiry rule. Both err towards the answer
        -- that does not silently lock a working contractor out over a date
        -- format nobody noticed; an expiry errs the other way because there the
        -- unsafe answer is a card that never dies.
        return true
    end

    return (now or Time.Now()) >= epoch

end

-- The daily status as it applies RIGHT NOW, which is not always the stored one:
-- a stored USING from yesterday is not today's state, and a card whose period
-- has ended reads EXPIRED whatever its last day said.
function ContractorUsage.status(employee, now)

    if not ContractorUsage.is_contractor(employee) then
        return nil
    end

    if ContractorUsage.is_expired(employee, now) then
        return ContractorUsage.STATUS.EXPIRED
    end

    if employee.usage_date ~= Time.Date(now) then
        return ContractorUsage.STATUS.NOT_STARTED
    end

    return employee.usage_status or ContractorUsage.STATUS.NOT_STARTED

end

-- Everything the frontend and the logs want about one contractor's day, in the
-- shape a JSON encoder can take straight.
function ContractorUsage.describe(employee, now)

    if not ContractorUsage.is_contractor(employee) then
        return nil
    end

    local window, bounds = ContractorUsage.window(now)

    return {
        card_code = employee.card_code,
        username = employee.username,
        status = ContractorUsage.status(employee, now),
        stored_status = employee.usage_status,
        usage_date = employee.usage_date,
        started_at = employee.usage_started_at,
        last_open_at = employee.usage_last_open_at,
        completed_at = employee.usage_completed_at,
        start_at = employee.start_at,
        expire_at = employee.expire_at,
        window = window,
        morning = clockOf(bounds.morning_start) .. "-" .. clockOf(bounds.morning_end),
        afternoon = clockOf(bounds.afternoon_start) .. "-" .. clockOf(bounds.afternoon_end),
        overtime = bounds.overtime,
        enforced = settings().usage_timeout_enabled == true,
        -- How the day is finished, and how far into that gesture this card is
        -- right now (0 unless they are mid-sequence at the reader).
        complete_scans = completeTarget(),
        complete_timeout = completeTimeout(),
        scan_count = sequence.card == employee.card_code and sequence.count or 0,
    }

end

------------------------------------------------------------
-- The gate (CardScanPlan sections 8 and 11)
------------------------------------------------------------

-- ContractorUsage.check_daily_usage(employee [, now]) -> allowed, reason, event
--
-- Called for CONTRACTORS ONLY, and only after the card has already passed the
-- active and expiry checks -- this answers "may they open it today", never "is
-- the card valid".
--
-- What it deliberately does NOT refuse:
--
--   * a second open on a day already COMPLETED. Somebody who came back for a
--     forgotten phone is not committing an offence, and the plan asks for no
--     such refusal. The day stays COMPLETED and the open is recorded.
--   * anything at all when contractor.usage_timeout_enabled is false, which is
--     the shipped default and the setting the plan itself shows.
function ContractorUsage.check_daily_usage(employee, now)

    if not ContractorUsage.is_contractor(employee) then
        return true
    end

    now = now or Time.Now()

    if settings().enforce_start_date ~= false and not ContractorUsage.has_started(employee, now) then

        stats.denied_not_started = stats.denied_not_started + 1

        return false,
               "the contractor period starts on " .. tostring(employee.start_at),
               Logger.EVENTS.USAGE_NOT_STARTED

    end

    if settings().usage_timeout_enabled ~= true then
        return true
    end

    local window, bounds = ContractorUsage.window(now)

    local function refuse(detail)

        stats.denied_out_of_hours = stats.denied_out_of_hours + 1

        return false, detail, Logger.EVENTS.USAGE_OUT_OF_HOURS

    end

    if window == ContractorUsage.WINDOW.BEFORE then
        return refuse("before the morning window opens at " .. clockOf(bounds.morning_start))
    end

    if window == ContractorUsage.WINDOW.MIDDAY then
        return refuse("between the morning window (ends " .. clockOf(bounds.morning_end) ..
                      ") and the afternoon window (opens " .. clockOf(bounds.afternoon_start) .. ")")
    end

    if window == ContractorUsage.WINDOW.AFTER and settings().allow_ot == false then
        return refuse("after the afternoon window closed at " .. clockOf(bounds.afternoon_end) ..
                      " and overtime is not allowed")
    end

    return true

end

------------------------------------------------------------
-- Recording (CardScanPlan section 1)
------------------------------------------------------------

local function write(employee, fields)

    local updated = EmployeeDb.update(employee.id, fields)

    if updated == nil then
        Logger.warning("could not record contractor usage for " .. tostring(employee.card_code))
        return employee
    end

    return updated

end

-- ContractorUsage.mark_used(employee [, now, locker]) -> row, changed
--
-- Every open lands here. Three cases, and the middle one is the whole point of
-- the redesign:
--
--   already USING today   only usage_last_open_at moves. A contractor who opens
--                         their locker six times has used it six times and
--                         finished nothing.
--   COMPLETED today       back to USING. They finished, then came back -- their
--                         period has not ended, so the locker is theirs and the
--                         day is open again until they finish it again.
--   any other day/state   today's record starts fresh.
--
-- `changed` is true when the STATUS moved, which is what decides whether the
-- caller has anything to announce.
function ContractorUsage.mark_used(employee, now, locker)

    now = now or Time.Now()

    local today = Time.Date(now)
    local stamp = Time.Stamp(now)

    if employee.usage_date == today then

        if employee.usage_status == ContractorUsage.STATUS.USING then
            return write(employee, { usage_last_open_at = stamp }), false
        end

        local fields = {
            usage_last_open_at = stamp,
            usage_status = ContractorUsage.STATUS.USING,
            -- The day is no longer finished, so the stamp that said it was must
            -- go; mark_completed writes a new one when they finish again.
            usage_completed_at = Database.NULL,
        }

        local reopened = employee.usage_status == ContractorUsage.STATUS.COMPLETED

        -- A day that somehow has no start (a record repaired by hand, or an
        -- EXPIRED sweep landing on today) gets one now rather than staying a
        -- day that ended without beginning.
        if employee.usage_started_at == nil then
            fields.usage_started_at = stamp
            stats.started = stats.started + 1
        end

        local row = write(employee, fields)

        if reopened then
            stats.reopened = stats.reopened + 1
        end

        Logger.contractor_usage(row, ContractorUsage.STATUS.USING,
                                reopened and "reopened after finishing" or "open on " .. today,
                                locker)

        return row, true

    end

    stats.started = stats.started + 1

    local row = write(employee, {
        usage_date = today,
        usage_started_at = stamp,
        usage_last_open_at = stamp,
        -- A new day clears the previous day's completion stamp; leaving it
        -- would show today as finished before it began.
        usage_completed_at = Database.NULL,
        usage_status = ContractorUsage.STATUS.USING,
    })

    Logger.contractor_usage(row, ContractorUsage.STATUS.USING, "first open of " .. today, locker)

    return row, true

end

-- ContractorUsage.mark_completed(employee [, now, locker]) -> row, changed
--
-- The finishing gesture landed. A day that was never started is started and
-- finished at once -- a contractor whose only interaction is the closing
-- sequence still had a usage day, and recording it as COMPLETED with no start
-- would leave a hole in the audit.
function ContractorUsage.mark_completed(employee, now, locker)

    now = now or Time.Now()

    local today = Time.Date(now)
    local stamp = Time.Stamp(now)

    if employee.usage_date == today and employee.usage_status == ContractorUsage.STATUS.COMPLETED then
        return write(employee, { usage_last_open_at = stamp }), false
    end

    local fields = {
        usage_date = today,
        usage_last_open_at = stamp,
        usage_completed_at = stamp,
        usage_status = ContractorUsage.STATUS.COMPLETED,
    }

    if employee.usage_date ~= today or employee.usage_started_at == nil then
        fields.usage_started_at = stamp
        stats.started = stats.started + 1
    end

    stats.completed = stats.completed + 1

    local row = write(employee, fields)

    Logger.contractor_usage(row, ContractorUsage.STATUS.COMPLETED,
                            completeTarget() .. " scans in a row on " .. today, locker)

    return row, true

end

-- ContractorUsage.on_locker_open(employee [, locker, now]) -> row, status, changed
--
-- Called by core/access.lua once the unlock pulse has actually gone out -- one
-- call per successful open, which is also one call per scan, which is what the
-- finishing gesture counts.
--
-- The locker opens either way. The last scan of the sequence is still a scan by
-- somebody entitled to their locker, and refusing to open it because they also
-- meant "I am done" would be a strange way to end the day.
function ContractorUsage.on_locker_open(employee, locker, now)

    if not ContractorUsage.is_contractor(employee) then
        return employee, nil, false
    end

    now = now or Time.Now()

    local _, finishes = ContractorUsage.note_scan(employee.card_code, now)

    local row, changed

    if finishes then
        row, changed = ContractorUsage.mark_completed(employee, now, locker)
    else
        row, changed = ContractorUsage.mark_used(employee, now, locker)
    end

    return row, row.usage_status, changed

end

------------------------------------------------------------
-- End of day
------------------------------------------------------------

-- Closes out every usage record whose day is over. A day left in USING becomes
-- EXPIRED -- the plan's "morning OPEN, no afternoon OPEN, end of day, still
-- considered USED". A COMPLETED day is left as it is: it is an honest record of
-- a finished shift, and the next open of a new day resets it anyway.
--
-- Idempotent and cheap, so it is safe to run hourly rather than trying to fire
-- exactly at midnight -- a gateway that was switched off overnight still closes
-- yesterday out when it comes back.
function ContractorUsage.end_of_day(now)

    now = now or Time.Now()

    local today = Time.Date(now)
    local closed = 0

    for _, row in ipairs(EmployeeDb.stale_usage(today, { ContractorUsage.STATUS.USING })) do

        local updated = write(row, { usage_status = ContractorUsage.STATUS.EXPIRED })

        closed = closed + 1
        stats.expired = stats.expired + 1

        Logger.contractor_usage(updated, ContractorUsage.STATUS.EXPIRED,
                                "day " .. tostring(row.usage_date) .. " ended without a closing open")

    end

    if closed > 0 then
        Logger.info("contractor usage: closed " .. closed .. " day(s) that ended while still in use")
    end

    return closed

end

------------------------------------------------------------
-- Reclaiming a finished contractor's locker
------------------------------------------------------------

-- ContractorUsage.is_reclaimable(employee [, now]) -> boolean, reason
--
-- BOTH conditions, and the second one is what makes this safe:
--
--   the card's PERIOD has ended    they have no further claim on the locker
--   the last day was COMPLETED     they performed the finishing gesture, which
--                                  is the only evidence this system has that
--                                  they took their belongings with them
--
-- A contractor whose period ended while the day was still USING -- or whose day
-- was swept to EXPIRED overnight because they never finished it -- is NOT
-- reclaimable, however long ago that was. Their locker may still have a bag in
-- it, and emptying it on a timer is not a decision software should make. That
-- case is what the admin override exists for (CardScanPlan section 4): a person
-- opens it, looks inside, and decides.
function ContractorUsage.is_reclaimable(employee, now)

    if not ContractorUsage.is_contractor(employee) then
        return false, "not a contractor"
    end

    if not ContractorUsage.is_expired(employee, now) then
        return false, "the card period has not ended"
    end

    if employee.usage_status ~= ContractorUsage.STATUS.COMPLETED then
        return false, "the last day was " .. tostring(employee.usage_status) ..
                      ", not " .. ContractorUsage.STATUS.COMPLETED
    end

    return true, "period ended on " .. tostring(employee.expire_at) ..
                 " and the last day (" .. tostring(employee.usage_date) .. ") was completed"

end

-- Every contractor whose locker may be taken back, with the reason, so a caller
-- logs WHY rather than only that it happened.
function ContractorUsage.reclaimable(now)

    now = now or Time.Now()

    local out = {}

    for _, row in ipairs(EmployeeDb.by_role(Employee.ROLE.CONTRACTOR)) do

        local ok, reason = ContractorUsage.is_reclaimable(row, now)

        if ok then
            out[#out + 1] = { employee = row, reason = reason }
        end

    end

    return out

end

------------------------------------------------------------
-- Housekeeping
------------------------------------------------------------

function ContractorUsage.init()

    local bounds = ContractorUsage.boundaries()
    local target = completeTarget()
    local timeout = completeTimeout()

    ContractorUsage.reset_sequence()

    Logger.info("contractor day: " ..
                clockOf(bounds.morning_start) .. "-" .. clockOf(bounds.morning_end) .. " / " ..
                clockOf(bounds.afternoon_start) .. "-" .. clockOf(bounds.afternoon_end) ..
                (settings().usage_timeout_enabled == true and " (hours enforced)" or " (hours not enforced)") ..
                (settings().allow_ot == false and ", no overtime" or ", overtime allowed"))

    if target == 0 then
        Logger.warning("contractor.complete_scan_count is 0; no scan sequence can finish a day, " ..
                       "so every day will end EXPIRED overnight")
    else
        Logger.info("contractor day ends on " .. target .. " scans in a row" ..
                    (timeout > 0 and (" within " .. timeout .. "s of each other")
                                  or " (no time limit between scans)"))
    end

    -- The same trap the admin sequence has: the reader drops a repeat of the
    -- same card within reader.repeat_ignore_ms, so a gap allowance shorter than
    -- that makes the sequence impossible to perform.
    local repeatIgnore = (Config.reader and tonumber(Config.reader.repeat_ignore_ms) or 0) / 1000

    if target > 1 and timeout > 0 and repeatIgnore >= timeout then
        Logger.warning("contractor.complete_scan_timeout (" .. timeout .. "s) is not longer than " ..
                       "reader.repeat_ignore_ms (" .. repeatIgnore .. "s); the finishing sequence " ..
                       "cannot be completed")
    end

    -- Yesterday, if the gateway was down when it ended.
    ContractorUsage.end_of_day()

    return true

end

function ContractorUsage.stats()
    return stats
end

function ContractorUsage.reset_stats()

    stats = {
        started = 0,
        completed = 0,
        reopened = 0,
        expired = 0,
        denied_not_started = 0,
        denied_out_of_hours = 0,
    }

    return stats

end

return ContractorUsage
