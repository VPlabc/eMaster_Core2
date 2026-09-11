-- tests/harness.lua
-- Shared plumbing for the Stage 1 and Stage 2 test scripts.
--
-- Output goes to both the console (print) and the gateway's runtime log
-- (Log.Info), because a test is run either from a terminal or from the web
-- Scripts page and only one of those is watching at a time.
--
-- These are hardware acceptance tests, not unit tests: most of them need a PLC,
-- a reader or a server at the other end. A step that cannot be attempted is
-- reported SKIP rather than FAIL, so a genuine failure is never hidden in a
-- column of red caused by unplugged equipment.

local Harness = {}

local results = { pass = 0, fail = 0, skip = 0 }
local title = "test"

local function emit(line)

    print(line)

    if type(Log) == "table" and type(Log.Info) == "function" then
        Log.Info(line)
    end

end

Harness.print = emit

function Harness.start(name)

    title = name
    results = { pass = 0, fail = 0, skip = 0 }

    emit("")
    emit("========================================================")
    emit("  " .. name)
    emit("========================================================")

end

function Harness.section(name)
    emit("")
    emit("-- " .. name .. " " .. string.rep("-", math.max(0, 50 - #name)))
end

function Harness.info(message)
    emit("   " .. message)
end

-- Harness.check(name, condition [, detail])
function Harness.check(name, condition, detail)

    if condition then
        results.pass = results.pass + 1
        emit("[ PASS ] " .. name .. (detail and ("  -- " .. tostring(detail)) or ""))
    else
        results.fail = results.fail + 1
        emit("[ FAIL ] " .. name .. (detail and ("  -- " .. tostring(detail)) or ""))
    end

    return condition

end

function Harness.skip(name, reason)

    results.skip = results.skip + 1
    emit("[ SKIP ] " .. name .. (reason and ("  -- " .. tostring(reason)) or ""))

    return false

end

-- Runs `fn` and fails the check if it raises, instead of ending the whole test
-- script on the first surprise.
function Harness.attempt(name, fn)

    local ok, result = pcall(fn)

    if not ok then
        Harness.check(name, false, tostring(result))
        return nil
    end

    return result

end

function Harness.finish()

    emit("")
    emit("--------------------------------------------------------")
    emit(string.format("  %s: %d passed, %d failed, %d skipped",
                       title, results.pass, results.fail, results.skip))
    emit("--------------------------------------------------------")
    emit("")

    return results

end

function Harness.results()
    return results
end

-- A database path that is never the live one. Every test that writes rows uses
-- this, so running the suite cannot lose a real locker assignment. Resolved by
-- Db.Open against the gateway's config directory, same as smartlocker.db.
Harness.TEST_DATABASE = "test_smartlocker.db"

-- Opens the test database and empties it. Returns true, or false plus the
-- reason -- a test that cannot get a clean database should report that and stop
-- rather than run against whatever was left behind.
function Harness.fresh_database()

    local Database = require("database.database")

    local ok, err = Database.open(Harness.TEST_DATABASE)

    if not ok then
        return false, err
    end

    Database.reset()

    return true

end

return Harness
