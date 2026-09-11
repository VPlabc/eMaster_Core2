-- core/access.lua
-- What happens when somebody swipes a card (Plan sections 8, 9, 10 and 30).
--
-- Every decision is made against the LOCAL database (Plan rule 5 and 6): no
-- REST call and no broker round trip stands between a valid card and its
-- locker. A server that is down, or a broker that is unreachable, delays
-- updates -- it never keeps somebody out of their locker.
--
-- The function returns as soon as the unlock pulse has been fired. Watching the
-- door open and close afterwards is core/locker.lua's state machine, driven
-- from the main loop, so a second person at another cabinet is served while the
-- first one is still putting their jacket away.

local Access = {}

local AdminAccess     = require("core.admin_access")
local Assignment      = require("core.assignment")
local Buzzer          = require("hardware.buzzer")
local Config          = require("config")
local ContractorUsage = require("core.contractor_usage")
local Employee        = require("core.employee")
local Locker          = require("core.locker")
local LockerDb        = require("database.locker_db")
local Logger          = require("utils.logger")
local Reader          = require("hardware.zk_reader")

local EVENTS = Logger.EVENTS

local stats = {
    scans = 0,
    granted = 0,
    denied = 0,
    not_found = 0,
    inactive = 0,
    expired = 0,
    wrong_type = 0,
    no_locker = 0,
    admin = 0,
    override = 0,
    usage = 0,
}

local lastResult = nil

------------------------------------------------------------
-- Denial helper
------------------------------------------------------------

local function deny(event, context, sound)

    stats.denied = stats.denied + 1

    Logger.access_denied(event, context)

    if sound then
        sound()
    end

    local result = {
        granted = false,
        event = event,
        reason = context.reason,
        card_code = context.card_code,
        username = context.username,
        locker = context.locker,
    }

    lastResult = result

    return result

end

------------------------------------------------------------
-- The block a swipe happened at
------------------------------------------------------------

-- Plan section 8's "Determine Current Locker Block". The reader reports which
-- door/reader produced the swipe; reader.block_by_door maps that onto a block.
-- With one cabinet the answer is always reader.default_block, and the type
-- check below then never fires -- which is correct, not a check that was
-- skipped.
local function blockFor(context)

    local blockId = context.block_id or Config.reader.default_block or 1

    return blockId, Config.Block(blockId)

end

------------------------------------------------------------
-- Access.on_card
------------------------------------------------------------

-- Access.on_card(card_code [, context]) -> result
--
-- context.block_id  which cabinet the card was presented at
-- context.source    "zk" / "card_api" / "manual", for the logs
function Access.on_card(cardCode, context)

    context = context or {}
    stats.scans = stats.scans + 1

    if cardCode == nil or cardCode == "" then
        return deny(EVENTS.ACCESS_DENIED, { reason = "empty card code" }, Buzzer.unknown_card)
    end

    local blockId, block = blockFor(context)

    Logger.card_scan(cardCode, blockId)

    -- 0. Admin first (CardScanPlan section 8). Before the employee lookup, and
    --    before every check below it: an admin card is not a person with a
    --    locker, and asking "is this card expired" of one is meaningless.
    if AdminAccess.is_admin_card(cardCode) then

        stats.admin = stats.admin + 1
        lastResult = AdminAccess.on_admin_scan(cardCode, { block_id = blockId, source = context.source })

        return lastResult

    end

    -- 0b. A user card while the override is open belongs to the admin machine:
    --     it opens THAT person's locker whatever their card's state is
    --     (section 5), and the mode stays open for the next person.
    if AdminAccess.is_override_active() then

        stats.override = stats.override + 1
        lastResult = AdminAccess.process_user_card(cardCode, { block_id = blockId, block = block })

        return lastResult

    end

    -- Somebody else's card in the middle of an admin scan sequence breaks it;
    -- their own swipe is then served normally, below.
    AdminAccess.note_other_card(cardCode)

    -- 1. Known card?
    local employee = Employee.find_by_card(cardCode)

    if employee == nil then

        stats.not_found = stats.not_found + 1

        return deny(EVENTS.CARD_NOT_FOUND, {
            card_code = cardCode,
            reason = "card is not in the local database",
        }, Buzzer.unknown_card)

    end

    -- 2. Active?
    if not Employee.is_active(employee) then

        stats.inactive = stats.inactive + 1

        return deny(EVENTS.CARD_INACTIVE, {
            card_code = employee.card_code,
            username = employee.username,
            reason = "card is inactive",
        }, Buzzer.denied)

    end

    -- 3. Expired? (section 10 -- long beep, and the locker goes EXPIRED so the
    --    status page shows yellow rather than green.)
    if Employee.is_expired(employee) then

        stats.expired = stats.expired + 1

        local held = LockerDb.get_by_card(employee.card_code)

        if held ~= nil and held.status ~= LockerDb.STATUS.ERROR then
            LockerDb.set_status(held.id, LockerDb.STATUS.EXPIRED)
        end

        return deny(EVENTS.CARD_EXPIRED, {
            card_code = employee.card_code,
            username = employee.username,
            locker = held,
            reason = "expired on " .. tostring(employee.expire_at),
        }, Buzzer.expired_card)

    end

    -- 3b. Contractor daily usage (CardScanPlan sections 1 and 11). Only a
    --     contractor reaches a decision here; an employee's card has no working
    --     day attached to it and check_daily_usage says so immediately.
    --
    --     This is deliberately AFTER the expiry check: a card whose period has
    --     ended is refused as expired, with the long beep the plan asks for,
    --     and never as "outside working hours" -- which would send the person
    --     back at 17:00 to be refused again.
    local usageOk, usageReason, usageEvent = ContractorUsage.check_daily_usage(employee)

    if not usageOk then

        stats.usage = stats.usage + 1

        return deny(usageEvent or EVENTS.ACCESS_DENIED, {
            card_code = employee.card_code,
            username = employee.username,
            locker = LockerDb.get_by_card(employee.card_code),
            reason = usageReason,
        }, Buzzer.denied)

    end

    -- 4. Which locker is theirs?
    local locker = LockerDb.get_by_card(employee.card_code)

    if locker == nil then

        -- Nobody assigned them one yet -- a card that arrived while every
        -- locker was taken, or a deployment that assigns on first use. Try now,
        -- so the person in front of the cabinet is served rather than told to
        -- come back after tomorrow's sync.
        if Config.locker.assign_on_scan ~= false then
            locker = Assignment.assign(employee, { block_id = block and block.id or nil })
        end

        if locker == nil then

            stats.no_locker = stats.no_locker + 1

            return deny(EVENTS.NO_LOCKER_ASSIGNED, {
                card_code = employee.card_code,
                username = employee.username,
                reason = "no locker is assigned and none is free",
            }, Buzzer.denied)

        end

    end

    -- 5. Right kind of locker, and the right cabinet?
    if not Assignment.type_matches(employee, locker) then

        -- Section 9: the person's own locker is of the wrong type for them.
        stats.wrong_type = stats.wrong_type + 1

        return deny(EVENTS.WRONG_LOCKER_TYPE, {
            card_code = employee.card_code,
            username = employee.username,
            locker = locker,
            reason = "locker " .. tostring(locker.locker_number) .. " is a " ..
                     tostring(locker.locker_type) .. " locker, the card is " .. tostring(employee.role),
        }, Buzzer.wrong_locker)

    end

    if block ~= nil and block.type ~= nil and block.type ~= locker.locker_type then

        -- Section 9's example: an employee holding employee locker 5 swipes at
        -- the contractor cabinet. Their locker is elsewhere and nothing here
        -- may open.
        stats.wrong_type = stats.wrong_type + 1

        return deny(EVENTS.WRONG_LOCKER_TYPE, {
            card_code = employee.card_code,
            username = employee.username,
            locker = locker,
            reason = "presented at the " .. tostring(block.type) .. " block, locker is " ..
                     tostring(locker.locker_type),
        }, Buzzer.wrong_locker)

    end

    if Config.locker.enforce_block and block ~= nil and locker.block_id ~= block.id then

        stats.wrong_type = stats.wrong_type + 1

        return deny(EVENTS.WRONG_LOCKER_TYPE, {
            card_code = employee.card_code,
            username = employee.username,
            locker = locker,
            reason = "locker is in block " .. tostring(locker.block_id) ..
                     ", card presented at block " .. tostring(block.id),
        }, Buzzer.wrong_locker)

    end

    -- 6. Already opening? A second swipe while the door is mid-cycle is the
    --    person being impatient, not a new request; re-pulsing the output would
    --    restart the timers and could re-latch a door that is already open.
    if Locker.busy(locker.id) then

        Logger.info("locker " .. tostring(locker.locker_number) .. " is already in " ..
                    Locker.runtime_state(locker.id) .. "; ignoring the repeat swipe")

        lastResult = { granted = true, event = EVENTS.ACCESS_GRANTED, card_code = employee.card_code,
                       locker = locker, repeated = true }

        return lastResult

    end

    -- An ERROR locker still belongs to this person and may well hold their
    -- belongings: it is opened, with the fault recorded. A clean open/close
    -- cycle puts it back to ASSIGNED on its own.
    if locker.status == LockerDb.STATUS.ERROR then
        Logger.warning("opening locker " .. tostring(locker.locker_number) ..
                       " which is in ERROR (" .. tostring(locker.last_error) .. ")")
    end

    -- 7. Unlock.
    local ok, err = Locker.unlock(locker.id, employee.card_code)

    if not ok then

        return deny(EVENTS.LOCKER_ERROR, {
            card_code = employee.card_code,
            username = employee.username,
            locker = locker,
            reason = tostring(err),
        }, Buzzer.door_error)

    end

    stats.granted = stats.granted + 1

    Logger.access_granted({
        card_code = employee.card_code,
        username = employee.username,
        locker = locker,
    })

    -- The day is recorded only once the pulse has actually gone out: a
    -- contractor whose locker refused to open has not used it, and marking them
    -- USING would be a record of something that did not happen.
    local usageStatus, usageChanged = nil, false

    if ContractorUsage.is_contractor(employee) then
        employee, usageStatus, usageChanged = ContractorUsage.on_locker_open(employee, locker)
    end

    -- The finishing sequence gets its own signal. Without it the third scan
    -- sounds exactly like the first two, and the contractor has no way to know
    -- whether the gesture registered -- which is the difference between a day
    -- that closes and one that expires overnight.
    if usageChanged and usageStatus == ContractorUsage.STATUS.COMPLETED then
        Buzzer.day_complete()
    else
        Buzzer.granted()
    end

    lastResult = {
        granted = true,
        event = EVENTS.ACCESS_GRANTED,
        card_code = employee.card_code,
        username = employee.username,
        locker = locker,
        usage_status = usageStatus,
    }

    return lastResult

end

------------------------------------------------------------
-- Main-loop entry point
------------------------------------------------------------

-- Reads whatever the reader has and runs it through on_card. Returns the result
-- when a card was handled, nil when there was nothing to do.
function Access.poll()

    local card = Reader.poll()

    if card == nil then
        return nil
    end

    return Access.on_card(card.card_code, {
        block_id = card.block_id,
        source = card.source,
        door = card.door,
    })

end

function Access.stats()
    return stats
end

function Access.last_result()
    return lastResult
end

return Access
