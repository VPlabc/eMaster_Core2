-- main.lua
-- SmartLocker Gateway (Plan section 35).
--
-- Coordination only. There is no Modbus frame, no SQL, no HTTP and no card
-- validation in this file -- if any of that ever appears here, it belongs in
-- the module that already owns it.
--
-- The loop below is the whole application:
--
--     begin a Modbus scan   -- one read per register, shared by every locker
--     read the card reader  -> core/access
--     step the door timers  -> core/locker
--     drain RabbitMQ        -> core/sync
--     run scheduled jobs    -> daily synchronisation, expiry, health
--     publish the UI state  -> api/frontend
--     sleep
--
-- Every step is called through pcall: one failing step must never end the run
-- (Plan section 36). A step that raises is logged once per minute rather than
-- once per pass, so an unreachable PLC does not bury the log.

-- Resolves this project's modules whether the script was started from its own
-- folder (the Scripts list, or lua.script_path = "smartlocker/main.lua") or
-- from the scripts root (the Lua Editor's Run button, which compiles the buffer
-- with no file behind it). See README, "Running".
do
    local dir = (debug.getinfo(1, "S").source or ""):match("^@(.*)[/\\][^/\\]+$")
    local roots = {}

    if dir then
        roots[#roots + 1] = (dir:gsub("[/\\]tests$", ""))
    end

    for entry in package.path:gmatch("[^;]+") do
        local base = entry:match("^(.*)%?%.lua$")
        if base then
            roots[#roots + 1] = base .. "smartlocker"
        end
    end

    local prefix = ""

    for _, root in ipairs(roots) do
        prefix = prefix .. root .. "/?.lua;" .. root .. "/?/init.lua;"
    end

    package.path = prefix .. package.path
end

local Access          = require("core.access")
local AdminAccess     = require("core.admin_access")
local Assignment      = require("core.assignment")
local Buzzer          = require("hardware.buzzer")
local Config          = require("config")
local ContractorUsage = require("core.contractor_usage")
local Database        = require("database.database")
local Employee        = require("core.employee")
local Frontend        = require("api.frontend")
local Locker          = require("core.locker")
local Logger          = require("utils.logger")
local Modbus          = require("hardware.modbus")
local RabbitMQ        = require("api.rabbitmq")
local Reader          = require("hardware.zk_reader")
local Scheduler       = require("utils.scheduler")
local Server          = require("api.server_api")
local Sync            = require("core.sync")
local Time            = require("utils.time")

------------------------------------------------------------
-- Error containment
------------------------------------------------------------

local complaints = {}

-- Runs `fn`, swallowing and reporting anything it raises. The same failure is
-- reported at most once a minute: a PLC that has gone away fails on every pass
-- of a 100 ms loop, and 600 identical log lines a minute would hide everything
-- else.
local function guard(name, fn)

    local ok, err = pcall(fn)

    if ok then
        complaints[name] = nil
        return true
    end

    local now = Time.Now()

    if complaints[name] == nil or (now - complaints[name]) > 60 then

        complaints[name] = now
        Logger.system("STEP_FAILED", "main loop step '" .. name .. "' failed", tostring(err), "ERROR")

    end

    return false

end

------------------------------------------------------------
-- Start-up (Plan section 37)
------------------------------------------------------------

local health = { plc = false, reader = false, mq = false, database = false }

local function startup()

    Logger.system("STARTUP", "SmartLocker starting -- " .. tostring(Config.system.name) ..
                             " (" .. tostring(Config.system.machine_id) .. ")",
                  "configuration: " .. tostring(Config.SourcePath() or "built-in defaults"))

    if Config.LoadError() then
        -- The application runs on the defaults, but a site whose timings are
        -- being ignored has to hear about it.
        Logger.system("CONFIG_ERROR", Config.LoadError(), nil, "ERROR")
    end

    -- 1. Database. Without the Db.* binding there is nowhere to put an
    --    employee or a locker, so this is the one failure that stops the run.
    if type(Db) ~= "table" or type(Db.Open) ~= "function" then
        Logger.system("STARTUP_FAILED",
                      "this gateway build has no Db.* binding -- SmartLocker needs SQLite",
                      nil, "ERROR")
        return false
    end

    local ok, err = Database.open()

    if not ok then
        Logger.system("STARTUP_FAILED", "cannot open the local database: " .. tostring(err), nil, "ERROR")
        return false
    end

    health.database = true

    -- 2. Lockers, from the configured layout
    ok, err = Locker.init()

    if not ok then
        Logger.system("STARTUP_FAILED", tostring(err), nil, "ERROR")
        return false
    end

    Employee.init()

    -- Access-control policy, both halves of CardScanPlan: the contractor's
    -- working day (which also closes out yesterday, if the gateway was off when
    -- it ended) and the admin card list. Neither touches hardware, so both run
    -- before the PLC is even tried.
    ContractorUsage.init()
    AdminAccess.init()

    -- The floor plan draws a countdown and needs to know which deadline
    -- applies; the policy lives in this script's configuration, so it is
    -- published where the web layer already reads.
    Assignment.publish_policy()

    -- 3. Hardware. Neither of these is fatal: a gateway that cannot reach the
    --    PLC still has to run, report the fault and recover when it comes back
    --    (Plan section 36).
    ok, err = Modbus.connect()
    health.plc = ok == true

    if not ok then
        Logger.system("PLC_UNAVAILABLE", "PLC not reachable at start-up: " .. tostring(err), nil, "WARNING")
    end

    ok, err = Reader.connect()
    health.reader = ok == true

    if not ok then
        Logger.reader_error("card reader not available at start-up: " .. tostring(err), "startup")
    end

    local buzzer = Buzzer.status()

    Logger.info("buzzer: " .. (buzzer.usable
        and ("enabled (" .. tostring(buzzer.backend) .. ")")
        or "disabled -- no output configured"))

    -- 4. Server and broker
    Server.configure()
    Sync.subscribe()
    health.mq = RabbitMQ.is_connected()

    -- 5. The web layer. Reads go straight to SQLite from the gateway's locker
    --    UI; these routes are the ACTIONS it sends back -- remote unlock,
    --    release, sync now. The hooks are handed over here rather than
    --    required inside api/frontend.lua, which would close a module cycle
    --    through core/sync -> core/assignment -> api/frontend.
    Frontend.set_hooks({
        release = function(lockerId, reason)
            return Assignment.release_locker(lockerId, reason)
        end,
        sync = function()
            return Sync.trigger()
        end,
        sync_status = function()
            return Sync.status()
        end,
        admin_status = function()
            return AdminAccess.status()
        end,
        admin_exit = function(reason)
            return AdminAccess.exit_override_mode(reason or "closed from the dashboard")
        end,
        admin_unlock_expired = function(lockerId)
            return AdminAccess.unlock_expired(lockerId)
        end,
        set_overtime = function(value)
            return ContractorUsage.set_overtime(value)
        end,
        health = function()
            return {
                plc = health.plc,
                reader = health.reader,
                mq = health.mq,
                database = health.database,
            }
        end,
    })

    Frontend.register_routes()

    -- 6. Make the stored state agree with reality before serving anybody:
    --    repair assignments, re-check expiry, then read the real door states.
    Assignment.reconcile()
    Locker.refresh_expiry()

    -- At start-up too, not only on the hourly timer: a gateway that was off for
    -- a week comes back with a week of finished contractors -- and, in hold
    -- mode, a week of lapsed holds -- still occupying lockers, and the first
    -- person at the cabinet should not have to wait for the top of the hour.
    Assignment.reclaim()

    if health.plc then
        Locker.reconcile()
    else
        Logger.warning("skipping door-state recovery: the PLC is unreachable")
    end

    -- 7. Jobs
    Sync.schedule()

    Scheduler.every("health", 15, function()

        health.plc = Modbus.is_connected()
        health.reader = Reader.is_connected()
        health.mq = RabbitMQ.is_connected()

        return true

    end)

    -- Contractor days that ended while still in use (CardScanPlan section 1).
    -- Hourly rather than "at midnight": the job is idempotent, it only touches
    -- records whose usage_date is already in the past, and an hourly sweep
    -- closes yesterday out even on a gateway that was switched off at midnight
    -- -- which a job scheduled for 00:00 would simply have missed.
    Scheduler.every("contractor-usage", 3600, function()

        ContractorUsage.end_of_day()

        -- Ordered, not merely adjacent: end_of_day() is what settles a day into
        -- its final state, and the reclaim below reads that state. Reclaiming
        -- first would judge a contractor on a day the sweep had not closed yet.
        Assignment.reclaim()

        return true

    end, { run_at_start = false })

    -- SQLite commits as it goes, so there is nothing to flush. What the
    -- database does need is a bound on the two audit tables, which are the only
    -- ones that grow.
    Scheduler.every("log-retention", 86400, function()

        Database.prune_logs(Config.database.retention_days)

        return true

    end)

    local summary = Locker.summary()

    Logger.system("STARTED",
                  "SmartLocker ready: " .. summary.total .. " lockers (" ..
                  (summary.EMPTY or 0) .. " empty, " .. (summary.ASSIGNED or 0) .. " assigned, " ..
                  (summary.EXPIRED or 0) .. " expired, " .. (summary.ERROR or 0) .. " error), " ..
                  #Employee.all() .. " cards")

    Frontend.publish(health, true)

    return true

end

------------------------------------------------------------
-- Main loop (Plan section 35)
------------------------------------------------------------

local function main()

    if not startup() then
        Logger.error("SmartLocker did not start; the script is stopping")
        return
    end

    local interval = Config.Value("system.loop_interval_ms", 100)
    local lastCard, lastResult = nil, nil

    while true do

        -- One scan: every register is read at most once per pass, however many
        -- lockers live in it.
        guard("modbus_scan", function()
            Modbus.begin_scan()
        end)

        guard("card_reader", function()

            local result = Access.poll()

            if result ~= nil then
                lastCard = result.card_code
                lastResult = result.event
                -- The UI should show a swipe immediately, not on the next
                -- publishing interval.
                Frontend.publish(health, true)
            end

        end)

        -- The admin machine is the one thing here that moves without a card:
        -- a half-entered scan sequence and an open ADMIN_ACCESS_MODE both have
        -- to lapse on their own (CardScanPlan section 6).
        guard("admin_timeout", function()
            AdminAccess.process_timeout()
        end)

        guard("door_monitor", function()
            Locker.poll()
        end)

        guard("rabbitmq", function()
            RabbitMQ.process()
        end)

        guard("scheduler", function()
            Scheduler.tick()
        end)

        guard("frontend", function()

            health.sync = Sync.status()
            health.last_card = lastCard
            health.last_result = lastResult
            health.admin = AdminAccess.status()

            Frontend.publish(health)

        end)

        Sleep(interval)

    end

end

main()
