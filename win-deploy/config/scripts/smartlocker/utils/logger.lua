-- utils/logger.lua
-- The one place this project writes a log line (Plan section 32).
--
-- Four destinations, one call:
--
--   * The runtime log (Log.Info/Warning/Error/Debug) -- the Log Viewer's
--     Runtime tab, a rotating file, gone after rotation. For a person watching
--     the gateway work.
--   * The structured store (Log.Write) -- config/logs.db, a real SQLite
--     database with per-type fields, searchable and paged on the Logs page.
--   * This application's OWN tables, locker_logs and system_logs (Plan section
--     15), in smartlocker.db. That is what the floor-plan dashboard reads: it
--     opens this database read-only and shows the newest rows as "Hoạt động
--     gần đây", and per-locker in the detail panel's Nhật ký tab.
--   * An in-memory ring of the most recent events, for a caller that wants the
--     last few without a query.
--
-- Two log databases is not duplication for its own sake: logs.db is the
-- gateway's cross-application record with a validated schema per type, and
-- smartlocker.db's tables are this application's own audit trail, joined to its
-- lockers and shipped to its UI. Losing either one should not cost the other.
--
-- Log.Write only accepts types declared in config/log_definitions.json. The
-- five this project uses are listed at the top of the README; a rejected write
-- is reported once, loudly, rather than silently dropping every business event
-- for the rest of the run.

local Gateway = Log

local Config = require("config")
local Time   = require("utils.time")

local Logger = {}

------------------------------------------------------------
-- Event names (Plan section 14)
------------------------------------------------------------

Logger.EVENTS = {
    CARD_SCAN           = "CARD_SCAN",
    ACCESS_GRANTED      = "ACCESS_GRANTED",
    ACCESS_DENIED       = "ACCESS_DENIED",
    CARD_NOT_FOUND      = "CARD_NOT_FOUND",
    CARD_INACTIVE       = "CARD_INACTIVE",
    CARD_EXPIRED        = "CARD_EXPIRED",
    WRONG_LOCKER_TYPE   = "WRONG_LOCKER_TYPE",
    NO_LOCKER_ASSIGNED  = "NO_LOCKER_ASSIGNED",
    LOCKER_ASSIGNED     = "LOCKER_ASSIGNED",
    LOCKER_RELEASED     = "LOCKER_RELEASED",
    LOCKER_UNLOCK       = "LOCKER_UNLOCK",
    DOOR_OPEN           = "DOOR_OPEN",
    DOOR_CLOSE          = "DOOR_CLOSE",
    DOOR_OPEN_TIMEOUT   = "DOOR_OPEN_TIMEOUT",
    DOOR_CLOSE_TIMEOUT  = "DOOR_CLOSE_TIMEOUT",
    LOCKER_ERROR        = "LOCKER_ERROR",
    EMPLOYEE_SYNC       = "EMPLOYEE_SYNC",
    RABBITMQ_UPDATE     = "RABBITMQ_UPDATE",
    PLC_ERROR           = "PLC_ERROR",
    READER_ERROR        = "READER_ERROR",
    SYSTEM              = "SYSTEM",

    -- Contractor daily usage (CardScanPlan section 1). The transition itself is
    -- CONTRACTOR_USAGE with the new state as its result; the two denials are
    -- separate events so the Logs page can tell "came before their period
    -- started" apart from "came outside the permitted hours".
    CONTRACTOR_USAGE    = "CONTRACTOR_USAGE",
    USAGE_NOT_STARTED   = "USAGE_NOT_STARTED",
    USAGE_OUT_OF_HOURS  = "USAGE_OUT_OF_HOURS",
    OVERTIME_UPDATED    = "OVERTIME_UPDATED",

    -- Admin access (CardScanPlan section 9). Every one of these is listed in
    -- the plan; they are kept as distinct events rather than one ADMIN event
    -- with a reason, because "who entered admin mode and when" is the question
    -- an audit actually asks.
    ADMIN_CARD_SCAN          = "ADMIN_CARD_SCAN",
    ADMIN_SCAN_SEQUENCE      = "ADMIN_SCAN_SEQUENCE",
    ADMIN_SCAN_TIMEOUT       = "ADMIN_SCAN_TIMEOUT",
    ADMIN_MODE_ENTER         = "ADMIN_MODE_ENTER",
    ADMIN_MODE_TIMEOUT       = "ADMIN_MODE_TIMEOUT",
    ADMIN_MODE_EXIT          = "ADMIN_MODE_EXIT",
    ADMIN_OVERRIDE_ACCESS    = "ADMIN_OVERRIDE_ACCESS",
    ADMIN_OVERRIDE_DENIED    = "ADMIN_OVERRIDE_DENIED",
    ADMIN_EXPIRED_LOCKER_OPEN = "ADMIN_EXPIRED_LOCKER_OPEN",
}

-- Structured log types, as declared in config/log_definitions.json.
Logger.TYPES = {
    ACCESS = "locker_access",
    DOOR   = "locker_door",
    ASSIGN = "locker_assign",
    SYNC   = "locker_sync",
    SYSTEM = "locker_system",
    -- Its own type rather than a flavour of ACCESS: an admin row carries two
    -- cards (whose override, and whose locker), and a row where card_code
    -- sometimes means the operator and sometimes the person is not a record
    -- anybody can filter on afterwards.
    ADMIN  = "locker_admin",
}

------------------------------------------------------------
-- Runtime log
------------------------------------------------------------

local PREFIX = "[SmartLocker] "

local function runtime(level, message)

    if type(Gateway) ~= "table" then
        print(PREFIX .. tostring(message))
        return
    end

    local write = Gateway[level]

    if type(write) == "function" then
        write(PREFIX .. tostring(message))
    end

end

function Logger.info(message)
    runtime("Info", message)
end

function Logger.warning(message)
    runtime("Warning", message)
end

function Logger.error(message)
    runtime("Error", message)
end

function Logger.debug(message)

    if Config.logging and Config.logging.debug then
        runtime("Debug", message)
    end

end

------------------------------------------------------------
-- Recent-event ring
------------------------------------------------------------

local recent = {}

local function remember(entry)

    local limit = (Config.logging and Config.logging.recent_limit) or 200

    recent[#recent + 1] = entry

    while #recent > limit do
        table.remove(recent, 1)
    end

end

-- Newest first, at most `count` entries -- the order a status page wants.
function Logger.recent(count)

    local out = {}
    local wanted = count or #recent

    for i = #recent, 1, -1 do

        if #out >= wanted then
            break
        end

        out[#out + 1] = recent[i]

    end

    return out

end

function Logger.clear_recent()
    recent = {}
end

------------------------------------------------------------
-- Structured events
------------------------------------------------------------

local rejectedTypes = {}

-- The database modules are required LAZILY, at first use rather than at load:
-- database/database.lua requires this file (it logs), so requiring it back here
-- at the top would be a cycle and one of the two would end up holding a
-- half-initialised table. By the time an event is logged both are loaded.
local Database = nil
local LockerDb = nil

local function store()

    if Database == nil then

        -- pcall'd: require() on a module that is itself mid-load raises
        -- ("loop or previous error"), and an event logged during start-up must
        -- not take the start-up down with it. The next call tries again.
        local ok, database = pcall(require, "database.database")
        local okLocker, lockers = pcall(require, "database.locker_db")

        if not ok or not okLocker then
            return nil
        end

        Database = database
        LockerDb = lockers

    end

    if not Database.is_open() then
        return nil
    end

    return Database

end

-- Which of this application's own tables an event belongs in. Access, door and
-- assignment events are about a locker and go to the audit trail the UI reads;
-- everything else is a gateway-level record.
local function audit(logType, fields, level)

    local database = store()

    if database == nil then
        return
    end

    local ok, err = pcall(function()

        if logType == Logger.TYPES.SYSTEM then

            database.insert("system_logs", {
                level = level,
                type = fields.event,
                message = fields.message or fields.detail,
                data_json = fields.reference,
            })

            -- A fault that names a locker also belongs on that locker's own
            -- timeline -- LOCKER_ERROR is exactly what someone looking at a red
            -- door needs to read.
            if fields.locker_id == nil then
                return
            end

        end

        LockerDb.log({
            locker_id = fields.locker_id,
            -- An admin row has no card_code of its own: it names the operator's
            -- card and, when there is one, the card whose locker was opened.
            -- The locker's own timeline wants whichever card the door was
            -- opened FOR, falling back to the admin who opened it.
            card_code = fields.card_code or fields.user_card or fields.admin_card,
            event = fields.event,
            result = fields.result or (level == "ERROR" and "ERROR" or nil),
            reason = fields.reason or fields.message,
        })

    end)

    if not ok then
        -- Never fatal: the audit trail is a record of the work, not a step in
        -- it. A locker still opens with a full disk.
        Logger.warning("audit row rejected: " .. tostring(err))
    end

end

-- Every business event goes through here. `fields` must only contain names
-- declared for `logType` in config/log_definitions.json -- the gateway
-- validates and rejects the row otherwise.
function Logger.event(logType, fields, level)

    fields = fields or {}
    level = level or "INFO"

    local entry = {
        type = logType,
        level = level,
        timestamp = Time.Stamp(),
    }

    for key, value in pairs(fields) do
        entry[key] = value
    end

    remember(entry)
    audit(logType, fields, level)

    if not (Config.logging and Config.logging.structured) then
        return true
    end

    if type(Gateway) ~= "table" or type(Gateway.Write) ~= "function" then
        return false, "Log.Write is not available"
    end

    -- nil fields are dropped rather than sent as empty strings: a required
    -- field that is genuinely missing should be rejected here, not stored as "".
    local payload = {}

    for key, value in pairs(fields) do
        if value ~= nil then
            payload[key] = value
        end
    end

    local ok, err = Gateway.Write(logType, payload, level)

    if not ok and not rejectedTypes[logType] then

        -- Reported once per type per run. A renamed field would otherwise
        -- produce a warning per card swipe for the life of the gateway.
        rejectedTypes[logType] = true
        Logger.warning("structured log type '" .. tostring(logType) .. "' rejected: " ..
                       tostring(err) .. " -- check config/log_definitions.json")

    end

    return ok, err

end

------------------------------------------------------------
-- Named events
------------------------------------------------------------

-- A locker reaches this module in one of two shapes: a database row, whose
-- primary key is `id`, or a configuration definition, which calls the same
-- number `locker_id`. Every log field goes through here so a row does not
-- silently log an empty locker_id.
local function lockerId(locker)

    if locker == nil then
        return nil
    end

    return locker.locker_id or locker.id

end

local function lockerFields(locker)

    if locker == nil then
        return {}
    end

    return {
        locker_id = lockerId(locker),
        locker_number = locker.locker_number,
        locker_type = locker.locker_type,
        block_id = locker.block_id,
    }

end

local function accessRow(event, result, context)

    context = context or {}

    local row = {
        event = event,
        result = result,
        reason = context.reason,
        card_code = context.card_code,
        username = context.username,
    }

    for key, value in pairs(lockerFields(context.locker)) do
        row[key] = value
    end

    return row

end

function Logger.card_scan(cardCode, blockId)

    Logger.info("card scan " .. tostring(cardCode) .. " at block " .. tostring(blockId))

    return Logger.event(Logger.TYPES.ACCESS, {
        event = Logger.EVENTS.CARD_SCAN,
        result = "SCAN",
        card_code = cardCode,
        block_id = blockId,
    })

end

function Logger.access_granted(context)

    Logger.info("ACCESS GRANTED " .. tostring(context.card_code) ..
                " -> locker " .. tostring(context.locker and context.locker.locker_number))

    return Logger.event(Logger.TYPES.ACCESS, accessRow(Logger.EVENTS.ACCESS_GRANTED, "GRANTED", context))

end

-- `event` is the specific denial (CARD_NOT_FOUND, CARD_EXPIRED, ...), so the
-- Logs page can filter on the cause; `result` stays DENIED for all of them.
function Logger.access_denied(event, context)

    Logger.warning("ACCESS DENIED (" .. tostring(event) .. ") " .. tostring(context.card_code) ..
                   (context.reason and (" -- " .. context.reason) or ""))

    return Logger.event(Logger.TYPES.ACCESS, accessRow(event, "DENIED", context), "WARNING")

end

------------------------------------------------------------
-- Contractor daily usage (CardScanPlan section 1)
------------------------------------------------------------

-- One row per state CHANGE, never per swipe: a contractor who opens their
-- locker four times in the afternoon has completed one day, and four identical
-- COMPLETED rows would say nothing the first one did not.
function Logger.contractor_usage(employee, status, reason, locker)

    Logger.info("contractor " .. tostring(employee and employee.card_code) ..
                " daily usage -> " .. tostring(status) ..
                (reason and (" (" .. reason .. ")") or ""))

    local row = accessRow(Logger.EVENTS.CONTRACTOR_USAGE, status, {
        card_code = employee and employee.card_code,
        username = employee and employee.username,
        reason = reason,
        locker = locker,
    })

    return Logger.event(Logger.TYPES.ACCESS, row)

end

------------------------------------------------------------
-- Admin access (CardScanPlan section 9)
------------------------------------------------------------

-- Logger.admin(event, fields [, level])
--
-- fields: admin_card, user_card, locker_id, locker_number, scan_count,
--         result, reason -- the field list declared for `locker_admin` in
--         config/log_definitions.json, and nothing else.
function Logger.admin(event, fields, level)

    fields = fields or {}
    fields.event = event

    local text = "admin: " .. tostring(event) ..
                 (fields.admin_card and (" [" .. tostring(fields.admin_card) .. "]") or "") ..
                 (fields.user_card and (" user " .. tostring(fields.user_card)) or "") ..
                 (fields.locker_number and (" locker " .. tostring(fields.locker_number)) or "") ..
                 (fields.reason and (" -- " .. tostring(fields.reason)) or "")

    if level == "ERROR" then
        Logger.error(text)
    elseif level == "WARNING" then
        Logger.warning(text)
    else
        Logger.info(text)
    end

    return Logger.event(Logger.TYPES.ADMIN, fields, level or "INFO")

end

function Logger.locker_assigned(employee, locker)

    Logger.info("locker " .. tostring(locker.locker_number) .. " (" .. tostring(locker.locker_type) ..
                ") assigned to " .. tostring(employee.card_code))

    return Logger.event(Logger.TYPES.ASSIGN, {
        event = Logger.EVENTS.LOCKER_ASSIGNED,
        card_code = employee.card_code,
        username = employee.username,
        locker_id = lockerId(locker),
        locker_number = locker.locker_number,
        locker_type = locker.locker_type,
        block_id = locker.block_id,
    })

end

function Logger.locker_released(locker, reason, cardCode)

    Logger.info("locker " .. tostring(locker and locker.locker_number) .. " released" ..
                (reason and (" -- " .. reason) or ""))

    return Logger.event(Logger.TYPES.ASSIGN, {
        event = Logger.EVENTS.LOCKER_RELEASED,
        card_code = cardCode or (locker and locker.card_code),
        locker_id = lockerId(locker),
        locker_number = locker and locker.locker_number,
        locker_type = locker and locker.locker_type,
        block_id = locker and locker.block_id,
        reason = reason,
    })

end

function Logger.assign_failed(employee, reason)

    Logger.warning("no locker for " .. tostring(employee.card_code) .. ": " .. tostring(reason))

    return Logger.event(Logger.TYPES.ASSIGN, {
        event = "LOCKER_ASSIGN_FAILED",
        card_code = employee.card_code,
        username = employee.username,
        locker_type = employee.role,
        reason = reason,
    }, "WARNING")

end

function Logger.door(event, locker, cardCode, durationSeconds, level)

    return Logger.event(Logger.TYPES.DOOR, {
        event = event,
        locker_id = lockerId(locker),
        locker_number = locker and locker.locker_number,
        block_id = locker and locker.block_id,
        card_code = cardCode,
        duration_s = durationSeconds,
    }, level)

end

function Logger.door_open(locker, cardCode, durationSeconds)
    Logger.info("door OPEN, locker " .. tostring(locker and locker.locker_number))
    return Logger.door(Logger.EVENTS.DOOR_OPEN, locker, cardCode, durationSeconds)
end

function Logger.door_close(locker, cardCode, durationSeconds)
    Logger.info("door CLOSED, locker " .. tostring(locker and locker.locker_number))
    return Logger.door(Logger.EVENTS.DOOR_CLOSE, locker, cardCode, durationSeconds)
end

function Logger.door_timeout(event, locker, cardCode, durationSeconds)
    Logger.error(tostring(event) .. ", locker " .. tostring(locker and locker.locker_number))
    return Logger.door(event, locker, cardCode, durationSeconds, "ERROR")
end

function Logger.locker_unlock(locker, cardCode)
    Logger.info("unlock pulse, locker " .. tostring(locker and locker.locker_number))
    return Logger.door(Logger.EVENTS.LOCKER_UNLOCK, locker, cardCode)
end

function Logger.locker_error(locker, reason, cardCode)

    Logger.error("locker " .. tostring(locker and locker.locker_number) .. " ERROR: " .. tostring(reason))

    return Logger.event(Logger.TYPES.SYSTEM, {
        event = Logger.EVENTS.LOCKER_ERROR,
        component = "locker",
        detail = reason,
        -- locker_id and card_code so this also lands on that door's own
        -- timeline in the dashboard, not only in the system log.
        locker_id = lockerId(locker),
        card_code = cardCode or (locker and locker.card_code),
        reference = tostring(locker and locker.locker_number or ""),
        message = "locker " .. tostring(locker and locker.locker_number) .. ": " .. tostring(reason),
    }, "ERROR")

end

function Logger.sync_result(source, stats, message, level)

    local text = string.format(
        "sync (%s): +%d new, %d updated, %d deactivated, %d assigned, %d released, %d errors",
        tostring(source), stats.added or 0, stats.updated or 0, stats.deactivated or 0,
        stats.assigned or 0, stats.released or 0, stats.errors or 0)

    if level == "ERROR" then
        Logger.error(text .. (message and (" -- " .. message) or ""))
    else
        Logger.info(text)
    end

    return Logger.event(Logger.TYPES.SYNC, {
        event = Logger.EVENTS.EMPLOYEE_SYNC,
        source = source,
        added = stats.added or 0,
        updated = stats.updated or 0,
        deactivated = stats.deactivated or 0,
        assigned = stats.assigned or 0,
        released = stats.released or 0,
        errors = stats.errors or 0,
        message = message,
    }, level or "INFO")

end

function Logger.mq_update(eventName, cardCode, result, message)

    return Logger.event(Logger.TYPES.SYNC, {
        event = Logger.EVENTS.RABBITMQ_UPDATE,
        source = eventName,
        card_code = cardCode,
        message = message,
        errors = (result == false) and 1 or 0,
    }, (result == false) and "WARNING" or "INFO")

end

function Logger.plc_error(operation, address, message)

    Logger.error("PLC " .. tostring(operation) .. " failed at " .. tostring(address) .. ": " .. tostring(message))

    return Logger.event(Logger.TYPES.SYSTEM, {
        event = Logger.EVENTS.PLC_ERROR,
        component = "plc",
        detail = operation,
        reference = tostring(address),
        message = message,
    }, "ERROR")

end

function Logger.reader_error(message, detail)

    Logger.error("reader: " .. tostring(message))

    return Logger.event(Logger.TYPES.SYSTEM, {
        event = Logger.EVENTS.READER_ERROR,
        component = "reader",
        detail = detail,
        message = message,
    }, "ERROR")

end

function Logger.system(event, message, detail, level)

    if level == "ERROR" then
        Logger.error(message)
    elseif level == "WARNING" then
        Logger.warning(message)
    else
        Logger.info(message)
    end

    return Logger.event(Logger.TYPES.SYSTEM, {
        event = event or Logger.EVENTS.SYSTEM,
        component = "gateway",
        detail = detail,
        message = message,
    }, level or "INFO")

end

return Logger
