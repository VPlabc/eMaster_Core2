-- api/frontend.lua
-- What the web layer can see and ask for (Plan sections 13 and 34).
--
-- Two channels, and the split between them is deliberate:
--
--   READS go straight to SQLite. The gateway's locker dashboard (its own port,
--   web.locker_ui_port) opens smartlocker.db read-only and queries it directly,
--   so drawing the floor plan costs this script nothing at all -- the page
--   stays live even while the script is busy waiting out a 30 second door
--   timeout. Nothing here serves those reads.
--
--   ACTIONS come back here as HTTP routes registered with Http.Register. A
--   remote unlock has to go through the state machine that owns the door: the
--   web thread must never pulse a coil or write a locker row itself. The
--   gateway hands the request to this script, the handler runs on the script's
--   own thread at its next Sleep(), and the answer goes back out the same
--   connection.
--
-- Routes (also reachable directly as /api/app/<path> on the gateway's main
-- port, which is how they can be tested with curl):
--
--   GET  lockers                     the same model the dashboard draws
--   GET  lockers/<id>                one locker plus its recent events
--   GET  employees                   card list
--   GET  status                      hardware/sync health
--   POST lockers/<id>/unlock         open a locker (the UI's "Mở khóa từ xa")
--   POST lockers/<id>/release        free a locker  (the UI's "Thu hồi")
--   POST sync                        run the employee synchronisation now
--   GET  admin                       admin card state machine (CardScanPlan 7)
--   POST admin/exit                  close ADMIN_ACCESS_MODE
--   POST admin/unlock/<id>           open one expired locker under the override
--   POST contractor/overtime?until=  extend tonight's afternoon window
--
-- Runtime variables are still published for the gateway's own dashboard, which
-- knows nothing about this application's schema.

local Config          = require("config")
local ContractorUsage = require("core.contractor_usage")
local Employee        = require("core.employee")
local EmployeeDb      = require("database.employee_db")
local Json            = require("utils.json")
local Locker          = require("core.locker")
local LockerDb        = require("database.locker_db")
local Logger          = require("utils.logger")
local Time            = require("utils.time")

local Frontend = {}

local lastPublish = 0
local registered = false

-- Set by main.lua so the action routes can reach the parts of the application
-- they trigger without this module requiring core/sync.lua (which requires
-- core/assignment.lua, which would close a cycle back through here).
local hooks = {}

function Frontend.set_hooks(table_)
    hooks = table_ or {}
end

local function prefix()
    return (Config.frontend and Config.frontend.variable_prefix) or "SL_"
end

local function publish(name, value)

    if type(SetVariable) ~= "function" then
        return
    end

    SetVariable(prefix() .. name, value)

end

------------------------------------------------------------
-- Queries
------------------------------------------------------------

-- Frontend.lockers([filter]) -- filter is nil, "empty", "assigned", "expired",
-- "error", or a locker type.
function Frontend.lockers(filter)

    local all = Locker.snapshot()

    if filter == nil or filter == "" or filter == "all" then
        return all
    end

    local wanted = tostring(filter):upper()
    local out = {}

    for _, locker in ipairs(all) do

        local matches = (locker.status == wanted)
            or (wanted == "USED" and locker.status == LockerDb.STATUS.ASSIGNED)
            or (wanted == "EMPLOYEE" and locker.locker_type == "employee")
            or (wanted == "CONTRACTOR" and locker.locker_type == "contractor")

        if matches then
            out[#out + 1] = locker
        end

    end

    return out

end

function Frontend.locker(lockerId)

    for _, locker in ipairs(Locker.snapshot()) do
        if locker.locker_id == lockerId then
            locker.logs = LockerDb.logs(lockerId, 50)
            return locker
        end
    end

    return nil

end

function Frontend.employees()

    local out = {}

    for _, row in ipairs(EmployeeDb.all()) do

        local locker = row.locker_id and LockerDb.get(row.locker_id) or nil

        out[#out + 1] = {
            id = row.id,
            username = row.username,
            card_code = row.card_code,
            role = row.role,
            gender = row.gender,
            expire_at = row.expire_at,
            start_at = row.start_at,
            active = row.active,
            status = Employee.status(row),
            -- The contractor's working day (CardScanPlan section 1), nil for an
            -- employee. Two different words: `status` is the card (ACTIVE /
            -- EXPIRED), `usage_status` is today.
            usage_status = ContractorUsage.status(row),
            usage_date = row.usage_date,
            usage_started_at = row.usage_started_at,
            usage_completed_at = row.usage_completed_at,
            locker_id = row.locker_id,
            locker_number = locker and locker.locker_number or nil,
            locker_type = locker and locker.locker_type or nil,
        }

    end

    return out

end

function Frontend.employee(cardCode)

    local row = EmployeeDb.find_by_card(cardCode)

    if row == nil then
        return nil
    end

    for _, entry in ipairs(Frontend.employees()) do
        if entry.id == row.id then
            -- One card, so the whole working day is worth the extra fields --
            -- which half of the day it is now, the windows in force, tonight's
            -- overtime. The list above deliberately carries only the summary.
            entry.usage = ContractorUsage.describe(row)
            return entry
        end
    end

    return nil

end

-- Recent locker events, newest first, straight from the audit table.
function Frontend.recent_events(count)
    return LockerDb.logs(nil, count or 25)
end

-- The working day as the dashboard shows it (CardScanPlan section 2). Read from
-- ContractorUsage rather than from Config directly, so the times shown are the
-- ones actually in force -- overtime included, and with the same fallbacks
-- applied when a boundary in the file is unreadable.
local function contractorDay()

    local window, bounds = ContractorUsage.window()
    local clock = ContractorUsage.clock_of

    return {
        window = window,
        morning = clock(bounds.morning_start) .. "-" .. clock(bounds.morning_end),
        afternoon = clock(bounds.afternoon_start) .. "-" .. clock(bounds.afternoon_end),
        overtime = bounds.overtime,
        enforced = (Config.contractor or {}).usage_timeout_enabled == true,
        allow_ot = (Config.contractor or {}).allow_ot ~= false,
    }

end

function Frontend.status()

    local summary = LockerDb.summary()

    return {
        machine_id = Config.system.machine_id,
        device_name = Config.system.name,
        updated_at = Time.Stamp(),
        lockers = {
            total = summary.total,
            empty = summary[LockerDb.STATUS.EMPTY] or 0,
            assigned = summary[LockerDb.STATUS.ASSIGNED] or 0,
            expired = summary[LockerDb.STATUS.EXPIRED] or 0,
            error = summary[LockerDb.STATUS.ERROR] or 0,
            open = summary.open or 0,
            no_output = summary.no_output or 0,
        },
        employees = {
            total = EmployeeDb.count(),
            active = #EmployeeDb.active(),
        },
        hardware = hooks.health and hooks.health() or {},
        sync = hooks.sync_status and hooks.sync_status() or {},
        admin = hooks.admin_status and hooks.admin_status() or {},
        contractor = contractorDay(),
    }

end

------------------------------------------------------------
-- HTTP routes
------------------------------------------------------------

local function reply(status, payload)
    return status, Json.encode(payload), "application/json"
end

local function ok(payload)

    payload = payload or {}
    payload.ok = true

    return reply(200, payload)

end

local function failed(status, message)
    return reply(status, { ok = false, error = message })
end

-- Path tail as a number: "lockers/7/unlock" -> 7.
local function lockerIdFrom(path)

    local id = tostring(path):match("^lockers/(%d+)")

    return id and math.tointeger(tonumber(id)) or nil

end

function Frontend.register_routes()

    if registered then
        return true
    end

    if type(Http) ~= "table" or type(Http.Register) ~= "function" then
        Logger.warning("this gateway build has no Http.Register binding; " ..
                       "the dashboard's action buttons will return 404")
        return false
    end

    Http.Register("GET", "lockers", function(request)
        return ok({ lockers = Frontend.lockers(request.query.status), summary = Frontend.status().lockers })
    end)

    Http.Register("GET", "employees", function()
        return ok({ employees = Frontend.employees() })
    end)

    Http.Register("GET", "status", function()
        return ok({ status = Frontend.status() })
    end)

    Http.Register("GET", "events", function(request)
        local limit = math.tointeger(tonumber(request.query.limit)) or 50
        return ok({ events = Frontend.recent_events(math.min(limit, 500)) })
    end)

    -- One locker, and the same path with a verb appended for the two actions.
    -- Registered per depth because that is how the gateway's route table is
    -- shaped (one, two or three segments below /api/app/).
    Http.Register("GET", "lockers/<id>", function(request)

        local locker = Frontend.locker(lockerIdFrom(request.path))

        if locker == nil then
            return failed(404, "no such locker")
        end

        return ok({ locker = locker })

    end)

    Http.Register("POST", "lockers/<id>/unlock", function(request)
        return Frontend.unlock(lockerIdFrom(request.path), request)
    end)

    Http.Register("POST", "lockers/<id>/release", function(request)
        return Frontend.release(lockerIdFrom(request.path), request)
    end)

    Http.Register("POST", "sync", function(request)
        return Frontend.sync(request)
    end)

    -- Admin access (CardScanPlan sections 4-6). The mode itself is entered at
    -- the reader, by card, and can only be entered there -- there is no route
    -- that opens it. What the dashboard gets is the two things a supervisor
    -- needs from a screen: see whether it is open, and close it.
    Http.Register("GET", "admin", function()
        return ok({ admin = hooks.admin_status and hooks.admin_status() or {} })
    end)

    Http.Register("POST", "admin/exit", function(request)
        return Frontend.admin_exit(request)
    end)

    Http.Register("POST", "admin/unlock/<id>", function(request)
        return Frontend.admin_unlock_expired(
            math.tointeger(tonumber(tostring(request.path):match("admin/unlock/(%d+)"))))
    end)

    -- The server extending tonight's shift (section 2). ?until=21:00, or no
    -- value at all to drop back to the configured afternoon_end.
    Http.Register("POST", "contractor/overtime", function(request)
        return Frontend.set_overtime(request)
    end)

    registered = true

    Logger.info("HTTP routes registered: " .. table.concat(Http.Routes(), ", "))

    return true

end

------------------------------------------------------------
-- Actions
------------------------------------------------------------

-- Remote unlock. Goes through the same Locker.unlock() a card swipe uses, so
-- the door monitor picks the cycle up, the timeouts apply, and the event lands
-- in the same audit trail with its source recorded.
function Frontend.unlock(lockerId, request)

    if lockerId == nil then
        return failed(400, "no locker id in the path")
    end

    local locker = LockerDb.get(lockerId)

    if locker == nil then
        return failed(404, "no such locker")
    end

    if not LockerDb.can_unlock(locker) then
        return failed(409, "locker " .. tostring(locker.locker_number) .. " has no unlock output configured")
    end

    if Locker.busy(lockerId) then
        return ok({ message = "locker " .. locker.locker_number .. " is already " ..
                               Locker.runtime_state(lockerId) })
    end

    local source = (request and request.query and request.query.source) or "api"

    local unlocked, err = Locker.unlock(lockerId, locker.card_code)

    if not unlocked then
        return failed(502, tostring(err))
    end

    Logger.event(Logger.TYPES.ACCESS, {
        event = Logger.EVENTS.ACCESS_GRANTED,
        result = "GRANTED",
        reason = "remote unlock (" .. source .. ")",
        card_code = locker.card_code,
        locker_id = lockerId,
        locker_number = locker.locker_number,
        locker_type = locker.locker_type,
        block_id = locker.block_id,
    })

    return ok({ message = "Đã mở hộc " .. tostring(locker.locker_number), locker_id = lockerId })

end

-- Frees a locker from the dashboard. The assignment module does the work, so
-- both halves of the relationship move together inside one transaction.
function Frontend.release(lockerId, request)

    if lockerId == nil then
        return failed(400, "no locker id in the path")
    end

    if hooks.release == nil then
        return failed(501, "release is not wired up in this build")
    end

    local locker = LockerDb.get(lockerId)

    if locker == nil then
        return failed(404, "no such locker")
    end

    if locker.card_code == nil then
        return ok({ message = "Hộc " .. tostring(locker.locker_number) .. " đã trống" })
    end

    local source = (request and request.query and request.query.source) or "api"
    local released, err = hooks.release(lockerId, "released from the dashboard (" .. source .. ")")

    if not released then
        return failed(500, tostring(err))
    end

    return ok({ message = "Đã thu hồi hộc " .. tostring(locker.locker_number) })

end

-- Closes ADMIN_ACCESS_MODE from the dashboard, for the case the plan's timeout
-- exists to cover but a supervisor noticed first.
function Frontend.admin_exit(request)

    if hooks.admin_exit == nil then
        return failed(501, "admin access is not wired up in this build")
    end

    local source = (request and request.query and request.query.source) or "api"
    local closed = hooks.admin_exit("closed from the dashboard (" .. source .. ")")

    if not closed then
        return ok({ message = "Chế độ admin không hoạt động", active = false })
    end

    return ok({ message = "Đã đóng chế độ admin", active = false })

end

-- Opens one expired locker under an override that is already open. Refuses when
-- it is not: this route is the dashboard's version of the admin's own scan, not
-- a way around the card sequence.
function Frontend.admin_unlock_expired(lockerId)

    if hooks.admin_unlock_expired == nil then
        return failed(501, "admin access is not wired up in this build")
    end

    if lockerId == nil then
        return failed(400, "no locker id in the path")
    end

    local status = hooks.admin_status and hooks.admin_status() or {}

    if status.override ~= true then
        return failed(409, "admin access mode is not open")
    end

    local outcome = hooks.admin_unlock_expired(lockerId)

    if outcome == nil or outcome.granted ~= true then
        return failed(409, (outcome and outcome.reason) or "the locker could not be opened")
    end

    return ok({
        message = "Đã mở hộc " .. tostring(outcome.locker and outcome.locker.locker_number),
        locker_id = lockerId,
        event = outcome.event,
    })

end

-- Extends (or clears) tonight's afternoon window.
function Frontend.set_overtime(request)

    if hooks.set_overtime == nil then
        return failed(501, "the contractor day is not wired up in this build")
    end

    local query = (request and request.query) or {}
    local value = query["until"] or query.value or query.time

    local applied, err = hooks.set_overtime(value)

    if not applied then
        return failed(400, tostring(err))
    end

    if value == nil or value == "" then
        return ok({ message = "Đã bỏ giờ tăng ca", overtime = Json.null })
    end

    return ok({ message = "Giờ tăng ca đến " .. tostring(value), overtime = value })

end

-- Runs the employee synchronisation now. Queued through the scheduler rather
-- than executed inline: a full REST sync can take seconds, and the HTTP client
-- is holding a connection.
function Frontend.sync(request)

    if hooks.sync == nil then
        return failed(501, "synchronisation is not wired up in this build")
    end

    local triggered = hooks.sync()

    if not triggered then
        return failed(500, "could not schedule the synchronisation")
    end

    return ok({ message = "Đã yêu cầu đồng bộ nhân sự" })

end

------------------------------------------------------------
-- Runtime variables (the gateway's own dashboard)
------------------------------------------------------------

-- The gateway's dashboard has no idea what a locker is; it shows named
-- variables. These are the summary an operator watching THAT page wants, not a
-- second copy of the model -- the locker UI reads the database directly.
function Frontend.publish(context, force)

    context = context or {}

    local interval = (Config.frontend and Config.frontend.publish_interval_sec) or 2

    if not force and (Time.Now() - lastPublish) < interval then
        return false
    end

    lastPublish = Time.Now()

    local summary = LockerDb.summary()

    publish("Status", Json.encode({
        machine_id = Config.system.machine_id,
        plc = context.plc == true,
        reader = context.reader == true,
        mq = context.mq == true,
        database = context.database ~= false,
        sync = context.sync,
        updated_at = Time.Stamp(),
    }))

    publish("PlcOk", context.plc == true)
    publish("ReaderOk", context.reader == true)
    publish("MqOk", context.mq == true)
    publish("LockersTotal", summary.total or 0)
    publish("LockersEmpty", summary[LockerDb.STATUS.EMPTY] or 0)
    publish("LockersAssigned", summary[LockerDb.STATUS.ASSIGNED] or 0)
    publish("LockersExpired", summary[LockerDb.STATUS.EXPIRED] or 0)
    publish("LockersError", summary[LockerDb.STATUS.ERROR] or 0)
    publish("LockersOpen", summary.open or 0)
    publish("Employees", EmployeeDb.count())

    if context.last_card ~= nil then
        publish("LastCard", tostring(context.last_card))
    end

    if context.last_result ~= nil then
        publish("LastResult", tostring(context.last_result))
    end

    -- Admin mode is worth a variable of its own: an operator watching the
    -- gateway's own dashboard should be able to see that a cabinet is sitting
    -- in override without opening the locker UI.
    if context.admin ~= nil then
        publish("AdminMode", context.admin.state or "NORMAL")
        publish("AdminOverride", context.admin.override == true)
    end

    return true

end

return Frontend
