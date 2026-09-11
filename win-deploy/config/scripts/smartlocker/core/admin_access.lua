-- core/admin_access.lua
-- The admin card state machine (CardScanPlan sections 3-9).
--
-- An admin card is NOT an employee record. It is recognised here, at the
-- access-control layer, before core/access.lua looks anybody up -- which is the
-- plan's own recommendation and is worth stating why: an admin card stored as
-- an employee would be handed a locker by the assignment pass, would expire,
-- and would be deactivated by the first synchronisation that did not mention
-- it. The list lives in the configuration instead, and nothing that reconciles
-- against the server can touch it.
--
-- The machine:
--
--     NORMAL
--       admin scan          -> ADMIN_SCAN 1..n-1     (each scan re-arms the
--                                                     scan_timeout)
--       n consecutive scans -> ADMIN_ACCESS_MODE
--       any other card      -> the sequence is broken; back to NORMAL and the
--                              card is served normally
--       scan_timeout        -> NORMAL
--
--     ADMIN_ACCESS_MODE
--       user card    -> open THAT person's locker, expiry and all (section 5)
--       admin scan   -> open the next expired locker (section 4), one door per
--                       scan, and extend the mode
--       override_timeout -> NORMAL
--
-- SECTION 4'S RULE ABOUT NOT OPENING EVERYTHING AT ONCE is why an admin scan
-- steps through the expired lockers rather than releasing them together: this
-- cabinet's latches are solenoids on one supply, and eighteen simultaneous
-- pulses is an inrush nobody sized the PLC for. It is also simply safer -- a
-- corridor of open doors is not a state an operator can supervise.
--
-- THE READER'S DE-BOUNCE MATTERS HERE. hardware/zk_reader.lua drops a repeat of
-- the same card within reader.repeat_ignore_ms (3 s by default), so five admin
-- scans are five separate presentations at least three seconds apart -- roughly
-- fifteen seconds in all, against a scan_timeout that is measured BETWEEN
-- scans, not across the sequence. Setting scan_timeout below repeat_ignore_ms
-- would make the sequence impossible to complete, and init() says so.

local Buzzer     = require("hardware.buzzer")
local Config     = require("config")
local Employee   = require("core.employee")
local Locker     = require("core.locker")
local LockerDb   = require("database.locker_db")
local Logger     = require("utils.logger")
local Time       = require("utils.time")

local AdminAccess = {}

local EVENTS = Logger.EVENTS

AdminAccess.STATE = {
    NORMAL   = "NORMAL",
    SCANNING = "ADMIN_SCAN",
    OVERRIDE = "ADMIN_ACCESS_MODE",
}

local state = {
    name = AdminAccess.STATE.NORMAL,
    scan_count = 0,
    admin_card = nil,
    last_scan_at = nil,
    deadline = nil,
    -- Lockers already opened in THIS override session, so a repeated admin scan
    -- walks along the expired ones instead of re-opening the first.
    opened = {},
    entered_at = nil,
}

local cardSet = nil

local stats = {
    admin_scans = 0,      -- admin cards presented
    sequences = 0,        -- completed 5-scan sequences
    overrides = 0,        -- times ADMIN_ACCESS_MODE was entered
    override_access = 0,  -- user lockers opened inside it
    expired_opened = 0,   -- expired lockers stepped through
    timeouts = 0,
    denied = 0,
}

------------------------------------------------------------
-- Settings
------------------------------------------------------------

local function settings()
    return Config.admin or {}
end

local function enabled()
    return settings().enabled == true
end

local function scanTarget()
    return math.max(1, math.tointeger(tonumber(settings().unlock_all_scan_count)) or 5)
end

local function scanTimeout()
    return tonumber(settings().scan_timeout) or 10
end

local function overrideTimeout()
    return tonumber(settings().override_timeout) or 60
end

-- Card codes are compared the way every other card in this application is:
-- trimmed and upper case. A configuration that lists "admin0001" and a reader
-- that reports "ADMIN0001" describe the same card.
local function normalize(cardCode)

    if cardCode == nil then
        return nil
    end

    local text = tostring(cardCode):gsub("%s+", ""):upper()

    if text == "" then
        return nil
    end

    return text

end

-- Built once and rebuilt by AdminAccess.reload(), not per swipe: this is asked
-- on every card read, before anything else.
local function cards()

    if cardSet ~= nil then
        return cardSet
    end

    cardSet = {}

    for _, code in ipairs(settings().cards or {}) do

        local normalized = normalize(code)

        if normalized then
            cardSet[normalized] = true
        end

    end

    return cardSet

end

function AdminAccess.reload()
    cardSet = nil
    return cards()
end

function AdminAccess.count()

    local total = 0

    for _ in pairs(cards()) do
        total = total + 1
    end

    return total

end

------------------------------------------------------------
-- State
------------------------------------------------------------

function AdminAccess.state()

    -- Reported as the plan writes it -- ADMIN_SCAN_3, not "SCANNING (3)" --
    -- because these strings go into the log and into the dashboard.
    if state.name == AdminAccess.STATE.SCANNING then
        return AdminAccess.STATE.SCANNING .. "_" .. tostring(state.scan_count)
    end

    return state.name

end

function AdminAccess.is_override_active()
    return state.name == AdminAccess.STATE.OVERRIDE
end

function AdminAccess.status()

    return {
        enabled = enabled(),
        state = AdminAccess.state(),
        scan_count = state.scan_count,
        scans_required = scanTarget(),
        admin_card = state.admin_card,
        override = AdminAccess.is_override_active(),
        entered_at = state.entered_at and Time.Stamp(state.entered_at) or nil,
        expires_in = state.deadline and math.max(0, state.deadline - Time.Now()) or nil,
        opened = AdminAccess.opened_count(),
        cards = AdminAccess.count(),
    }

end

function AdminAccess.opened_count()

    local total = 0

    for _ in pairs(state.opened) do
        total = total + 1
    end

    return total

end

local function toNormal()

    state.name = AdminAccess.STATE.NORMAL
    state.scan_count = 0
    state.admin_card = nil
    state.last_scan_at = nil
    state.deadline = nil
    state.entered_at = nil
    state.opened = {}

end

function AdminAccess.reset()
    toNormal()
end

------------------------------------------------------------
-- Results
------------------------------------------------------------

-- Admin results are shaped like core/access.lua's, so main.lua and the frontend
-- can treat "what happened to the last card" uniformly whoever handled it.
local function result(granted, event, fields)

    fields = fields or {}

    return {
        granted = granted,
        event = event,
        admin = true,
        admin_state = AdminAccess.state(),
        card_code = fields.card_code,
        username = fields.username,
        locker = fields.locker,
        reason = fields.reason,
        repeated = fields.repeated,
    }

end

------------------------------------------------------------
-- Entering and leaving the override
------------------------------------------------------------

function AdminAccess.enter_override_mode(adminCard)

    state.name = AdminAccess.STATE.OVERRIDE
    state.admin_card = adminCard or state.admin_card
    state.entered_at = Time.Now()
    state.deadline = Time.Deadline(overrideTimeout())
    state.opened = {}

    stats.overrides = stats.overrides + 1

    Logger.admin(EVENTS.ADMIN_MODE_ENTER, {
        admin_card = state.admin_card,
        scan_count = state.scan_count,
        result = "GRANTED",
        reason = "admin access mode is open for " .. overrideTimeout() .. "s",
    })

    Buzzer.admin_mode()

    return result(true, EVENTS.ADMIN_MODE_ENTER, {
        card_code = state.admin_card,
        reason = "admin access mode",
    })

end

-- AdminAccess.exit_override_mode([reason, event]) -> true | false
--
-- Returns false when there was nothing to leave, so a dashboard button can say
-- "admin mode was not active" instead of reporting a success that did nothing.
function AdminAccess.exit_override_mode(reason, event)

    if state.name == AdminAccess.STATE.NORMAL then
        return false
    end

    local wasOverride = AdminAccess.is_override_active()
    local adminCard = state.admin_card
    local opened = AdminAccess.opened_count()

    toNormal()

    if wasOverride then

        Logger.admin(event or EVENTS.ADMIN_MODE_EXIT, {
            admin_card = adminCard,
            result = "CLOSED",
            reason = (reason or "admin access mode closed") ..
                     (opened > 0 and (" -- " .. opened .. " locker(s) opened") or ""),
        })

        Buzzer.admin_exit()

    end

    return true

end

------------------------------------------------------------
-- Timeouts (CardScanPlan section 6)
------------------------------------------------------------

-- Called once per pass of the main loop. Nothing else moves this machine on its
-- own: a half-entered sequence and an open override both have to lapse without
-- anybody presenting a card, which is exactly the case the plan is worried
-- about (an admin who walked away).
function AdminAccess.process_timeout(now)

    if state.name == AdminAccess.STATE.NORMAL then
        return false
    end

    now = now or Time.Now()

    if state.name == AdminAccess.STATE.SCANNING then

        if state.last_scan_at ~= nil and (now - state.last_scan_at) > scanTimeout() then

            local adminCard, count = state.admin_card, state.scan_count

            toNormal()
            stats.timeouts = stats.timeouts + 1

            Logger.admin(EVENTS.ADMIN_SCAN_TIMEOUT, {
                admin_card = adminCard,
                scan_count = count,
                result = "RESET",
                reason = "more than " .. scanTimeout() .. "s between admin scans",
            })

            return true

        end

        return false

    end

    if state.deadline ~= nil and now >= state.deadline then

        stats.timeouts = stats.timeouts + 1

        AdminAccess.exit_override_mode(
            "no admin activity for " .. overrideTimeout() .. "s",
            EVENTS.ADMIN_MODE_TIMEOUT)

        return true

    end

    return false

end

------------------------------------------------------------
-- Is this an admin card?
------------------------------------------------------------

function AdminAccess.is_admin_card(cardCode)

    if not enabled() then
        return false
    end

    local code = normalize(cardCode)

    if code == nil then
        return false
    end

    return cards()[code] == true

end

-- core/access.lua calls this when it is about to serve a card that is NOT an
-- admin card and the override is not open. A sequence of admin scans has to be
-- consecutive (section 7 leaves ADMIN_SCAN_n only by another admin scan or by
-- timeout), so somebody else swiping in the middle of one ends it.
function AdminAccess.note_other_card(cardCode)

    if state.name ~= AdminAccess.STATE.SCANNING then
        return false
    end

    local adminCard, count = state.admin_card, state.scan_count

    toNormal()

    Logger.admin(EVENTS.ADMIN_SCAN_SEQUENCE, {
        admin_card = adminCard,
        user_card = normalize(cardCode),
        scan_count = count,
        result = "RESET",
        reason = "another card was presented before the sequence completed",
    })

    return true

end

------------------------------------------------------------
-- Expired lockers (CardScanPlan section 4)
------------------------------------------------------------

-- The expired lockers this session has not opened yet, in door order.
function AdminAccess.pending_expired()

    local pending = {}

    for _, locker in ipairs(LockerDb.by_status(LockerDb.STATUS.EXPIRED)) do

        if not state.opened[locker.id] and LockerDb.can_unlock(locker) then
            pending[#pending + 1] = locker
        end

    end

    return pending

end

-- AdminAccess.unlock_expired(lockerId) -> result
--
-- One door. Used by the dashboard, and by the scan-stepping below. It refuses
-- anything that is not EXPIRED: the override is for lockers whose holder's card
-- has lapsed, not a master key to the cabinet.
function AdminAccess.unlock_expired(lockerId)

    local locker = LockerDb.get(lockerId)

    if locker == nil then
        return result(false, EVENTS.ADMIN_OVERRIDE_DENIED, { reason = "no such locker" })
    end

    if locker.status ~= LockerDb.STATUS.EXPIRED then
        return result(false, EVENTS.ADMIN_OVERRIDE_DENIED, {
            locker = locker,
            reason = "locker " .. tostring(locker.locker_number) .. " is " .. tostring(locker.status) ..
                     ", not " .. LockerDb.STATUS.EXPIRED,
        })
    end

    if not LockerDb.can_unlock(locker) then
        return result(false, EVENTS.ADMIN_OVERRIDE_DENIED, {
            locker = locker,
            reason = "locker " .. tostring(locker.locker_number) .. " has no unlock output",
        })
    end

    if Locker.busy(locker.id) then
        return result(true, EVENTS.ADMIN_EXPIRED_LOCKER_OPEN, {
            locker = locker,
            repeated = true,
            reason = "already " .. Locker.runtime_state(locker.id),
        })
    end

    local ok, err = Locker.unlock(locker.id, locker.card_code)

    if not ok then

        stats.denied = stats.denied + 1

        Logger.admin(EVENTS.ADMIN_OVERRIDE_DENIED, {
            admin_card = state.admin_card,
            user_card = locker.card_code,
            locker_id = locker.id,
            locker_number = locker.locker_number,
            result = "ERROR",
            reason = tostring(err),
        }, "ERROR")

        Buzzer.door_error()

        return result(false, EVENTS.LOCKER_ERROR, { locker = locker, reason = tostring(err) })

    end

    state.opened[locker.id] = true
    stats.expired_opened = stats.expired_opened + 1

    Logger.admin(EVENTS.ADMIN_EXPIRED_LOCKER_OPEN, {
        admin_card = state.admin_card,
        user_card = locker.card_code,
        locker_id = locker.id,
        locker_number = locker.locker_number,
        result = "GRANTED",
        reason = "EXPIRED_CONTRACTOR",
    })

    Buzzer.granted()

    return result(true, EVENTS.ADMIN_EXPIRED_LOCKER_OPEN, {
        card_code = locker.card_code,
        locker = locker,
        reason = "expired locker opened under admin override",
    })

end

-- One scan, one door. Returns the plan's "no expired locker left" answer rather
-- than silently doing nothing, so the operator hears the difference.
function AdminAccess.unlock_next_expired()

    local pending = AdminAccess.pending_expired()

    if #pending == 0 then

        Logger.admin(EVENTS.ADMIN_OVERRIDE_DENIED, {
            admin_card = state.admin_card,
            result = "NONE",
            reason = "no expired locker left to open",
        })

        Buzzer.denied()

        return result(false, EVENTS.ADMIN_OVERRIDE_DENIED, {
            card_code = state.admin_card,
            reason = "no expired locker left to open",
        })

    end

    return AdminAccess.unlock_expired(pending[1].id)

end

------------------------------------------------------------
-- Admin scans (CardScanPlan sections 3, 4 and 7)
------------------------------------------------------------

-- AdminAccess.on_admin_scan(cardCode [, context]) -> result
--
-- Only called for a card is_admin_card() has already accepted.
function AdminAccess.on_admin_scan(cardCode, context)

    local code = normalize(cardCode)
    local now = Time.Now()

    stats.admin_scans = stats.admin_scans + 1

    Logger.admin(EVENTS.ADMIN_CARD_SCAN, {
        admin_card = code,
        scan_count = state.scan_count,
        result = "SCAN",
        reason = "state " .. AdminAccess.state(),
    })

    -- Already inside the override: this is the operator working, not entering.
    if AdminAccess.is_override_active() then

        state.deadline = Time.Deadline(overrideTimeout())
        state.last_scan_at = now

        if settings().scan_opens_expired == false then

            Logger.admin(EVENTS.ADMIN_SCAN_SEQUENCE, {
                admin_card = code,
                result = "EXTENDED",
                reason = "admin access mode extended by " .. overrideTimeout() .. "s",
            })

            Buzzer.admin_scan()

            return result(true, EVENTS.ADMIN_SCAN_SEQUENCE, {
                card_code = code,
                reason = "admin access mode extended",
            })

        end

        return AdminAccess.unlock_next_expired()

    end

    -- A different admin card mid-sequence starts the sequence again as that
    -- card's. Two operators half-scanning must not add up to an override
    -- neither of them completed.
    if state.name == AdminAccess.STATE.SCANNING and state.admin_card ~= code then

        Logger.admin(EVENTS.ADMIN_SCAN_SEQUENCE, {
            admin_card = code,
            scan_count = state.scan_count,
            result = "RESET",
            reason = "a different admin card interrupted the sequence",
        })

        state.scan_count = 0

    end

    state.name = AdminAccess.STATE.SCANNING
    state.admin_card = code
    state.last_scan_at = now
    state.scan_count = state.scan_count + 1

    local target = scanTarget()

    if state.scan_count >= target then

        stats.sequences = stats.sequences + 1

        Logger.admin(EVENTS.ADMIN_SCAN_SEQUENCE, {
            admin_card = code,
            scan_count = state.scan_count,
            result = "COMPLETE",
            reason = target .. " consecutive admin scans",
        })

        return AdminAccess.enter_override_mode(code)

    end

    Buzzer.admin_scan()

    return result(false, EVENTS.ADMIN_CARD_SCAN, {
        card_code = code,
        reason = "admin scan " .. state.scan_count .. " of " .. target,
    })

end

------------------------------------------------------------
-- A user card inside the override (CardScanPlan section 5)
------------------------------------------------------------

-- AdminAccess.process_user_card(cardCode [, context]) -> result
--
-- The point of the whole mode: the manager stands at the cabinet, the person
-- whose card has lapsed presents it, and their own locker opens. Expiry and
-- inactivity are ignored -- that is what the override IS -- but nothing else
-- is: an unknown card is still unknown, and a card with no locker still has no
-- locker to open. Neither of those is a fault the admin can fix by unlocking
-- something else.
--
-- The mode stays open afterwards, so the next person can be served (section 5).
function AdminAccess.process_user_card(cardCode, context)

    context = context or {}

    -- Every user card handled here is admin activity: it re-arms the mode.
    state.deadline = Time.Deadline(overrideTimeout())

    local employee = Employee.find_by_card(cardCode)

    local function refuse(event, reason, locker)

        stats.denied = stats.denied + 1

        Logger.admin(EVENTS.ADMIN_OVERRIDE_DENIED, {
            admin_card = state.admin_card,
            user_card = normalize(cardCode),
            locker_id = locker and locker.id or nil,
            locker_number = locker and locker.locker_number or nil,
            result = "DENIED",
            reason = reason,
        }, "WARNING")

        Buzzer.denied()

        return result(false, event, {
            card_code = normalize(cardCode),
            username = employee and employee.username or nil,
            locker = locker,
            reason = reason,
        })

    end

    if employee == nil then
        return refuse(EVENTS.CARD_NOT_FOUND, "card is not in the local database")
    end

    local expired = Employee.is_expired(employee)

    if (expired or not Employee.is_active(employee)) and settings().override_expired == false then
        return refuse(EVENTS.ACCESS_DENIED, "admin.override_expired is off; the card is " ..
                                            Employee.status(employee))
    end

    local locker = LockerDb.get_by_card(employee.card_code)

    if locker == nil then
        return refuse(EVENTS.NO_LOCKER_ASSIGNED, "no locker is assigned to this card")
    end

    if not LockerDb.can_unlock(locker) then
        return refuse(EVENTS.LOCKER_ERROR,
                      "locker " .. tostring(locker.locker_number) .. " has no unlock output", locker)
    end

    if Locker.busy(locker.id) then
        return result(true, EVENTS.ADMIN_OVERRIDE_ACCESS, {
            card_code = employee.card_code,
            username = employee.username,
            locker = locker,
            repeated = true,
            reason = "already " .. Locker.runtime_state(locker.id),
        })
    end

    local ok, err = Locker.unlock(locker.id, employee.card_code)

    if not ok then

        Buzzer.door_error()

        return refuse(EVENTS.LOCKER_ERROR, tostring(err), locker)

    end

    state.opened[locker.id] = true
    stats.override_access = stats.override_access + 1

    -- Why the override was needed, in the row itself. The plan's example log
    -- carries exactly this as `reason`.
    local why = expired and "EXPIRED_CONTRACTOR"
        or (not Employee.is_active(employee)) and "INACTIVE_CARD"
        or "ADMIN_REQUEST"

    Logger.admin(EVENTS.ADMIN_OVERRIDE_ACCESS, {
        admin_card = state.admin_card,
        user_card = employee.card_code,
        locker_id = locker.id,
        locker_number = locker.locker_number,
        result = "GRANTED",
        reason = why,
    })

    Buzzer.granted()

    return result(true, EVENTS.ADMIN_OVERRIDE_ACCESS, {
        card_code = employee.card_code,
        username = employee.username,
        locker = locker,
        reason = why,
    })

end

------------------------------------------------------------
-- Housekeeping
------------------------------------------------------------

function AdminAccess.init()

    AdminAccess.reload()
    toNormal()

    if not enabled() then
        Logger.info("admin cards: disabled")
        return true
    end

    local total = AdminAccess.count()

    if total == 0 then
        Logger.warning("admin.enabled is true but admin.cards is empty; no card can open admin mode")
    else
        Logger.info("admin cards: " .. total .. " configured, " .. scanTarget() ..
                    " scans to enter admin mode (" .. scanTimeout() .. "s between scans, " ..
                    overrideTimeout() .. "s mode timeout)")
    end

    -- The sequence is impossible when the reader drops repeats for longer than
    -- the gap the machine allows between them. Better said once at start-up
    -- than discovered by an operator scanning a card five times to no effect.
    local repeatIgnore = (Config.reader and tonumber(Config.reader.repeat_ignore_ms) or 0) / 1000

    if total > 0 and repeatIgnore >= scanTimeout() then
        Logger.warning("admin.scan_timeout (" .. scanTimeout() .. "s) is not longer than " ..
                       "reader.repeat_ignore_ms (" .. repeatIgnore .. "s); the admin scan " ..
                       "sequence cannot be completed")
    end

    return true

end

function AdminAccess.stats()
    return stats
end

return AdminAccess
