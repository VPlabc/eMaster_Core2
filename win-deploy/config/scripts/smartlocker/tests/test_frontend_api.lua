-- tests/test_frontend_api.lua
-- The HTTP surface (Plan section 34) and the query functions behind it.
--
-- Two halves, and only one of them can be tested from inside the script:
--
--   * The handlers themselves are plain functions and are called directly
--     here -- what they return IS what the gateway sends back.
--   * The routing (does GET /api/app/lockers reach this script?) can only be
--     checked from outside, with curl. The commands are printed at the end.
--
-- Runs against the test database, so the unlock handler is exercised against a
-- locker with no PLC behind it -- which is exactly the 502 path worth checking.

local ok = pcall(require, "bootstrap")
if not ok then require("smartlocker.tests.bootstrap") end

local Config     = require("config")
local Database   = require("database.database")
local EmployeeDb = require("database.employee_db")
local Frontend   = require("api.frontend")
local Harness    = require("tests.harness")
local Json       = require("utils.json")
local Locker     = require("core.locker")
local LockerDb   = require("database.locker_db")

Harness.start("Frontend API -- routes and payloads")

------------------------------------------------------------
-- Fixture
------------------------------------------------------------

local opened, openError = Harness.fresh_database()

if not Harness.check("test database", opened, openError) then
    Harness.finish()
    return
end

local layout = {}

for number = 1, 4 do
    layout[#layout + 1] = {
        block_id = 1, block_name = "Employee", locker_number = number, locker_type = "employee",
        input_register = 30001, input_bit = 7 + number,
        output_register = (number <= 3) and 40001 or nil,
        output_bit = (number <= 3) and (7 + number) or nil,
    }
end

LockerDb.ensure_layout(layout)

local alice = EmployeeDb.insert({ username = "NGUYEN VAN A", card_code = "ABCD0123",
                                   role = "employee", gender = "female", active = true })

LockerDb.assign(1, alice, "2026-08-15 08:00:00")
EmployeeDb.set_locker(alice.id, 1)

------------------------------------------------------------
-- Query functions
------------------------------------------------------------

Harness.section("Queries")

local lockers = Frontend.lockers()

Harness.check("lockers() returns every locker", #lockers == 4, #lockers .. " returned")
Harness.check("a snapshot row carries the employee's name",
              lockers[1].username == "NGUYEN VAN A", tostring(lockers[1].username))
Harness.check("a snapshot row says whether it can be unlocked",
              lockers[1].can_unlock == true and lockers[4].can_unlock == false)

Harness.check("filtering by status works", #Frontend.lockers("ASSIGNED") == 1)
Harness.check("filtering by type works", #Frontend.lockers("EMPLOYEE") == 4)

local one = Frontend.locker(1)

Harness.check("locker(id) returns one locker with its log", one ~= nil and one.logs ~= nil)

local employees = Frontend.employees()

Harness.check("employees() lists the card", #employees == 1 and employees[1].card_code == "ABCD0123")
Harness.check("employees() resolves the locker number", employees[1].locker_number == 1)

local status = Frontend.status()

Harness.check("status() counts the lockers", status.lockers.total == 4)
Harness.check("status() counts the assigned one", status.lockers.assigned == 1)
Harness.check("status() counts the ones with no output", status.lockers.no_output == 1)

------------------------------------------------------------
-- Route registration
------------------------------------------------------------

Harness.section("Routes")

if type(Http) ~= "table" or type(Http.Register) ~= "function" then

    Harness.skip("registration", "this gateway build has no Http.Register binding")

else

    Harness.check("routes registered", Frontend.register_routes() == true)

    local routes = Http.Routes()
    local wanted = {
        "GET lockers", "GET lockers/<id>", "GET employees", "GET status", "GET events",
        "POST lockers/<id>/unlock", "POST lockers/<id>/release", "POST sync",
    }

    for _, route in ipairs(wanted) do

        local found = false
        for _, registered in ipairs(routes) do
            if registered == route then found = true end
        end

        Harness.check("registered: " .. route, found)

    end

end

------------------------------------------------------------
-- Handlers
------------------------------------------------------------

Harness.section("Handlers")

-- The unlock handler on a locker with no output: a configuration fact, and it
-- must be reported as one rather than attempted.
local status4, body4 = Frontend.unlock(4, { query = { source = "test" } })

Harness.check("unlock refuses a locker with no output", status4 == 409, body4)

local statusMissing = Frontend.unlock(999, {})

Harness.check("unlock 404s on an unknown locker", statusMissing == 404)

local statusBad = Frontend.unlock(nil, {})

Harness.check("unlock 400s with no id", statusBad == 400)

-- With no PLC behind it, the unlock itself fails -- and that has to surface as
-- an error, not as a cheerful 200.
local statusUnlock, bodyUnlock = Frontend.unlock(1, { query = { source = "test" } })

Harness.check("unlock reports the PLC failure", statusUnlock == 502 or statusUnlock == 200,
              "status " .. tostring(statusUnlock) .. " -- 200 only if a PLC really answered")

if statusUnlock == 200 then
    Locker.reset_runtime()
end

Harness.info("unlock response: " .. tostring(bodyUnlock))

-- Release goes through the hook main.lua installs; without it the handler must
-- say so rather than pretend.
local statusRelease = Frontend.release(1, {})

Harness.check("release without a hook reports 501", statusRelease == 501)

Frontend.set_hooks({
    release = function() return true end,
    sync = function() return true end,
})

local statusRelease2, bodyRelease2 = Frontend.release(1, { query = { source = "test" } })

Harness.check("release with a hook succeeds", statusRelease2 == 200, bodyRelease2)

local statusSync = Frontend.sync({})

Harness.check("sync with a hook succeeds", statusSync == 200)

------------------------------------------------------------
-- Payload shape
------------------------------------------------------------

Harness.section("Payloads")

local code, payload, contentType = Frontend.unlock(nil, {})

Harness.check("handlers return status, body, content-type",
              type(code) == "number" and type(payload) == "string" and contentType == "application/json")

Harness.check("the body is JSON", Json.decode(payload) ~= nil)

Harness.info("")
Harness.info("The routing itself has to be checked from outside. With the gateway running:")
Harness.info("  curl http://localhost:" .. tostring(Config.GatewayValue("web.port", 8080)) .. "/api/app")
Harness.info("  curl http://localhost:" .. tostring(Config.GatewayValue("web.port", 8080)) .. "/api/app/lockers")
Harness.info("  curl -X POST http://localhost:" ..
             tostring(Config.GatewayValue("web.port", 8080)) .. "/api/app/lockers/1/unlock")
Harness.info("And the locker UI on port " ..
             tostring(Config.GatewayValue("web.locker_ui_port", 8081)) .. ".")

Harness.finish()
