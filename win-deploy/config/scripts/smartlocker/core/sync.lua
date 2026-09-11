-- core/sync.lua
-- Keeping the local database in step with the server (Plan sections 3 and 33).
--
-- Two paths, deliberately different in kind:
--
--   * RabbitMQ carries the realtime updates. One event, one card, applied the
--     moment it arrives.
--   * The daily REST call is the RECONCILIATION. It is the only thing that can
--     notice a card the broker never told us about, or one we were told about
--     twice, or a card that quietly disappeared from the server.
--
-- Neither is on the path of a card swipe. When both are down the gateway keeps
-- opening lockers from the database it already has (Plan section 3.2), and that
-- is the whole point of storing it locally.

local Assignment = require("core.assignment")
local Config     = require("config")
local Employee   = require("core.employee")
local Locker     = require("core.locker")
local Logger     = require("utils.logger")
local RabbitMQ   = require("api.rabbitmq")
local Scheduler  = require("utils.scheduler")
local Server     = require("api.server_api")
local Time       = require("utils.time")

local Sync = {}

local JOB = "employee-sync"

local state = {
    last_attempt = nil,
    last_success = nil,
    last_error = nil,
    runs = 0,
    failures = 0,
    mq_events = 0,
}

------------------------------------------------------------
-- Daily REST synchronisation
------------------------------------------------------------

-- Sync.run(source) -> ok, stats|error
--
-- Returns false on any failure; the scheduler then retries on
-- sync.retry_interval rather than waiting out another full day.
function Sync.run(source)

    source = source or "rest"
    state.last_attempt = Time.Stamp()
    state.runs = state.runs + 1

    local list, err = Server.get_active_cards()

    if list == nil then

        state.failures = state.failures + 1
        state.last_error = err

        Logger.sync_result(source, { errors = 1 }, err, "ERROR")

        return false, err

    end

    -- An empty list is refused as a full reconciliation. It is indistinguishable
    -- from a server that answered 200 with an empty envelope after a deploy --
    -- and acting on it would deactivate every card in the building and release
    -- every locker. If the site genuinely revoked everybody, it can be done from
    -- the server one card at a time, or by setting sync.allow_empty_list.
    local fullList = true

    if #list == 0 and not Config.sync.allow_empty_list then

        fullList = false

        Logger.warning("the server returned an EMPTY active-card list; treating it as suspect and " ..
                       "NOT deactivating anything (set sync.allow_empty_list to override)")

    end

    local stats, changes = Employee.sync(list, { full_list = fullList })

    local assignStats = Assignment.apply_changes(changes)

    -- Expiry moves on its own, with no event to announce it: a contractor whose
    -- last day was yesterday has to be EXPIRED today even though nothing about
    -- their record changed.
    Locker.refresh_expiry()
    Assignment.reconcile()

    -- A synchronisation is the moment expire_at actually moves, so it is also
    -- the moment a finished contractor becomes reclaimable. Waiting for the
    -- hourly sweep would hold a locker that the server just said is finished
    -- with.
    Assignment.reclaim()

    state.last_success = Time.Stamp()
    state.last_error = nil

    local merged = {
        added = stats.added,
        updated = stats.updated,
        deactivated = stats.deactivated,
        assigned = assignStats.assigned,
        released = assignStats.released,
        errors = stats.invalid + assignStats.failed,
    }

    Logger.sync_result(source, merged,
                       #list .. " cards received" .. (fullList and "" or " (partial: nothing deactivated)"))

    return true, merged

end

-- Registers the daily job (Plan section 3.1: interval configurable, default
-- 86400 seconds).
function Sync.schedule()

    Scheduler.every(JOB, Config.sync.interval or 86400, function()
        return Sync.run("rest")
    end, {
        run_at_start = Config.sync.run_at_start ~= false,
        retry_interval = Config.sync.retry_interval,
    })

    -- Expiry is re-evaluated hourly as well as on each sync, so a card that
    -- lapses at midnight is EXPIRED on the status page by 01:00 rather than at
    -- the next daily run.
    Scheduler.every("expiry-refresh", 3600, function()

        local changed = Locker.refresh_expiry()

        if changed > 0 then
            Logger.info("expiry refresh: " .. changed .. " locker(s) changed status")
        end

        return true

    end)

    return true

end

-- What an operator's "Sync now" would call.
function Sync.trigger()
    return Scheduler.trigger(JOB)
end

------------------------------------------------------------
-- RabbitMQ realtime updates (Plan section 3.2)
------------------------------------------------------------

local function eventKind(name)

    local events = Config.mq.events or {}

    if name == (events.created or "employee.card.created") then
        return "created"
    elseif name == (events.updated or "employee.card.updated") then
        return "updated"
    elseif name == (events.revoked or "employee.card.revoked") then
        return "revoked"
    end

    return nil

end

-- One broker event -> one local change. Never a full reconciliation: a single
-- message says nothing about the cards it does not mention.
function Sync.on_event(name, data)

    state.mq_events = state.mq_events + 1

    local kind = eventKind(name)

    if kind == nil then
        return false, "unknown event " .. tostring(name)
    end

    local record = Server.normalize(data)

    if record == nil or record.card_code == nil then
        Logger.mq_update(name, nil, false, "event carried no usable card record")
        return false, "no card record"
    end

    if kind == "revoked" then

        Employee.deactivate(record.card_code, "revoked by the server")

        if (Config.sync.on_removed or "release") == "release" then
            Assignment.release(record.card_code, "card revoked")
        end

        Logger.mq_update(name, record.card_code, true, "card revoked")

        return true

    end

    local row, action, err = Employee.upsert(record)

    if row == nil then
        Logger.mq_update(name, record.card_code, false, tostring(err))
        return false, err
    end

    local assigned = nil

    if row.active ~= false and not Employee.is_expired(row) and Config.sync.assign_on_sync ~= false then

        local locker = Assignment.assign(row)

        if locker ~= nil then
            assigned = locker.locker_number
        end

    end

    -- A card that became inactive through an update, rather than through a
    -- revoke event.
    if row.active == false and (Config.sync.on_removed or "release") == "release" then
        Assignment.release(row.card_code, "card deactivated by the server")
    end

    Locker.refresh_expiry()

    Logger.mq_update(name, row.card_code, true,
                     action .. (assigned and (", locker " .. assigned) or ""))

    return true

end

-- Wires the handler into the broker inbox. Called once at start-up; draining is
-- RabbitMQ.process(), called from the main loop.
function Sync.subscribe()

    RabbitMQ.subscribe(function(name, data)
        Sync.on_event(name, data)
    end)

    local ok, err = RabbitMQ.connect()

    if not ok then
        -- Not fatal: MqClient reconnects on its own and process() keeps being
        -- called. The daily REST sync is what covers the gap.
        Logger.warning("RabbitMQ not ready: " .. tostring(err) .. " -- realtime updates will start when it connects")
    end

    return ok

end

function Sync.status()

    return {
        last_attempt = state.last_attempt,
        last_success = state.last_success,
        last_error = state.last_error,
        runs = state.runs,
        failures = state.failures,
        mq_events = state.mq_events,
        interval = Config.sync.interval,
    }

end

return Sync
