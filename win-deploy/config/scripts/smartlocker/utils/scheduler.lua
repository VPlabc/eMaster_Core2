-- utils/scheduler.lua
-- Cooperative timers for the main loop.
--
-- A LuaEngine runs one script on one thread, and every native binding takes the
-- VM's mutex -- so there is no way to run the daily synchronisation "in the
-- background". What there is instead: a main loop that calls Scheduler.tick()
-- every pass and jobs that are due get their turn. A job therefore must not
-- block for long, because nothing else in the gateway's script -- card reads
-- included -- happens while it runs.
--
-- Every job is called through pcall. A job that raises is logged, marked, and
-- rescheduled; it never takes down the loop (Plan section 36: "The application
-- must not terminate because of a single external error").

local Logger = require("utils.logger")
local Time   = require("utils.time")

local Scheduler = {}

local jobs = {}
local order = {}

------------------------------------------------------------
-- Registration
------------------------------------------------------------

-- Scheduler.every(name, intervalSeconds, fn [, options])
--
--   options.run_at_start  run once on the first tick instead of waiting out a
--                         full interval (used by the daily sync, which should
--                         reconcile at boot rather than 24 hours later)
--   options.retry_interval  seconds to wait before the next run when the job
--                           returns false (a failed sync should come back in 15
--                           minutes, not tomorrow)
function Scheduler.every(name, intervalSeconds, fn, options)

    options = options or {}

    local job = {
        name = name,
        interval = intervalSeconds,
        fn = fn,
        retry_interval = options.retry_interval,
        next_run = options.run_at_start and 0 or Time.Deadline(intervalSeconds),
        runs = 0,
        failures = 0,
        last_run = nil,
        last_ok = nil,
        last_error = nil,
    }

    if jobs[name] == nil then
        order[#order + 1] = name
    end

    jobs[name] = job

    return job

end

-- One-shot.
function Scheduler.after(name, delaySeconds, fn)

    local job = Scheduler.every(name, delaySeconds, fn)

    job.once = true
    job.next_run = Time.Deadline(delaySeconds)

    return job

end

function Scheduler.remove(name)

    jobs[name] = nil

    for index, jobName in ipairs(order) do
        if jobName == name then
            table.remove(order, index)
            break
        end
    end

end

-- Makes a job due on the next tick. This is what an operator's "Sync now"
-- button, or a RabbitMQ event that asks for a full reconcile, should call --
-- never the job function directly, so the run still goes through the same
-- error handling and bookkeeping.
function Scheduler.trigger(name)

    local job = jobs[name]

    if job == nil then
        return false
    end

    job.next_run = 0
    return true

end

------------------------------------------------------------
-- Execution
------------------------------------------------------------

local function runJob(job)

    local now = Time.Now()

    job.runs = job.runs + 1
    job.last_run = now

    local ok, result = pcall(job.fn)

    if not ok then

        job.failures = job.failures + 1
        job.last_ok = false
        job.last_error = tostring(result)

        Logger.system("SCHEDULED_JOB_FAILED",
                      "scheduled job '" .. job.name .. "' raised an error",
                      tostring(result), "ERROR")

    else

        job.last_ok = (result ~= false)
        job.last_error = nil

        if result == false then
            job.failures = job.failures + 1
        end

    end

    if job.once then
        Scheduler.remove(job.name)
        return
    end

    -- A failed run comes back on the retry interval when one is configured;
    -- otherwise on the normal interval.
    local delay = job.interval

    if job.last_ok == false and job.retry_interval then
        delay = job.retry_interval
    end

    job.next_run = Time.Deadline(delay)

end

-- Runs whatever is due. Called once per pass of the main loop; cheap when
-- nothing is due (one os.time() plus a comparison per job).
function Scheduler.tick()

    local now = Time.Now()
    local ran = 0

    -- Iterated over a copy of the name list: a job is allowed to add or remove
    -- jobs (the one-shot retry does exactly that) without disturbing this walk.
    local names = {}

    for index, name in ipairs(order) do
        names[index] = name
    end

    for _, name in ipairs(names) do

        local job = jobs[name]

        if job and now >= job.next_run then
            runJob(job)
            ran = ran + 1
        end

    end

    return ran

end

------------------------------------------------------------
-- Introspection
------------------------------------------------------------

function Scheduler.status()

    local out = {}

    for _, name in ipairs(order) do

        local job = jobs[name]

        if job then
            out[#out + 1] = {
                name = job.name,
                interval = job.interval,
                runs = job.runs,
                failures = job.failures,
                last_run = job.last_run and Time.Stamp(job.last_run) or nil,
                last_ok = job.last_ok,
                last_error = job.last_error,
                next_run = job.next_run > 0 and Time.Stamp(job.next_run) or "now",
            }
        end

    end

    return out

end

function Scheduler.reset()
    jobs = {}
    order = {}
end

return Scheduler
