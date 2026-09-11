-- tests/test_server_api.lua
-- Stage 2 (Plan section 23): the backend REST API.
--
-- Nothing here writes to the database. It fetches the active-card list, checks
-- the shape of what came back, and reports how many records would survive
-- validation -- so a field-name mismatch between this server and
-- server.fields in smartlocker.json is found here rather than as "every card
-- is unknown" on the cabinet.

local ok = pcall(require, "bootstrap")
if not ok then require("smartlocker.tests.bootstrap") end

local Config   = require("config")
local Employee = require("core.employee")
local Harness  = require("tests.harness")
local Json     = require("utils.json")
local Server   = require("api.server_api")
local Time     = require("utils.time")

Harness.start("Stage 2 -- REST API")

local base = Config.server.base_url

if base == nil or base == "" then
    base = tostring(Config.GatewayValue("rest.url", "(not configured)")) .. "  [from the gateway configuration]"
end

Harness.info("Server:   " .. base)
Harness.info("Path:     " .. tostring(Config.server.active_cards_path))
Harness.info("Api key:  " .. ((Config.server.api_key ~= "" and Config.server.api_key ~= nil)
                              and "set in smartlocker.json" or "the gateway's own"))

Server.configure()

------------------------------------------------------------
-- Reachability
------------------------------------------------------------

Harness.section("Connection")

local reachable, statusOrError = Server.ping()

if not Harness.check("server answered", reachable, statusOrError) then

    Harness.info("Checked: is rest.url set on the Configuration page? Is the host reachable?")
    Harness.info("A failing REST server does NOT stop lockers from opening -- the gateway keeps")
    Harness.info("serving from its local database (Plan rule 6).")

    Harness.finish()
    return

end

Harness.check("HTTP status is a success", type(statusOrError) == "number" and statusOrError < 300,
              "status " .. tostring(statusOrError))

------------------------------------------------------------
-- The active-card list
------------------------------------------------------------

Harness.section("GET active cards")

local list, err = Server.get_active_cards()

if not Harness.check("card list retrieved", list ~= nil, err) then

    Harness.info("Common causes:")
    Harness.info("  * the array is under a key not listed in server.list_keys")
    Harness.info("  * the endpoint needs an api key (server.api_key)")
    Harness.info("  * the body is not JSON")

    Harness.finish()
    return

end

Harness.check("list is not empty", #list > 0,
              #list .. " record(s) -- an empty list is refused as a full reconciliation by core/sync")

------------------------------------------------------------
-- Field mapping and validation
------------------------------------------------------------

Harness.section("Record shape")

local counts = { valid = 0, invalid = 0, employees = 0, contractors = 0,
                 female = 0, male = 0, no_gender = 0, expired = 0, no_expiry = 0, inactive = 0 }

local firstProblem = nil

for _, record in ipairs(list) do

    local validated, reason = Employee.validate(record)

    if validated == nil then

        counts.invalid = counts.invalid + 1
        firstProblem = firstProblem or reason

    else

        counts.valid = counts.valid + 1

        if validated.role == Employee.ROLE.CONTRACTOR then
            counts.contractors = counts.contractors + 1
        else
            counts.employees = counts.employees + 1
        end

        if validated.gender == Employee.GENDER.FEMALE then
            counts.female = counts.female + 1
        elseif validated.gender == Employee.GENDER.MALE then
            counts.male = counts.male + 1
        else
            counts.no_gender = counts.no_gender + 1
        end

        if validated.expire_at == nil then
            counts.no_expiry = counts.no_expiry + 1
        elseif Time.IsExpired(validated.expire_at, nil, Config.sync.expire_at_end_of_day) then
            counts.expired = counts.expired + 1
        end

        if not validated.active then
            counts.inactive = counts.inactive + 1
        end

    end

end

Harness.check("every record validates", counts.invalid == 0,
              counts.invalid .. " rejected" .. (firstProblem and (" -- first: " .. firstProblem) or ""))

Harness.info(string.format("valid=%d  employees=%d  contractors=%d", counts.valid, counts.employees, counts.contractors))
Harness.info(string.format("female=%d  male=%d  gender missing=%d", counts.female, counts.male, counts.no_gender))
Harness.info(string.format("expired=%d  no expiry=%d  inactive=%d", counts.expired, counts.no_expiry, counts.inactive))

-- Section 2's rule, checked against the real data rather than assumed.
local contractorsWithoutExpiry = 0

for _, record in ipairs(list) do
    if Employee.normalize_role(record.role) == Employee.ROLE.CONTRACTOR and record.expire_at == nil then
        contractorsWithoutExpiry = contractorsWithoutExpiry + 1
    end
end

Harness.check("every contractor has an expiry (Plan section 2)", contractorsWithoutExpiry == 0,
              contractorsWithoutExpiry .. " contractor(s) with no expire_at -- those cards never expire")

Harness.check("gender is present (needed for the female-priority rule)", counts.no_gender == 0,
              counts.no_gender .. " record(s) with no usable gender")

if #list > 0 then
    Harness.info("")
    Harness.info("First record as the gateway sees it:")
    Harness.info(Json.encode(list[1], true))
end

------------------------------------------------------------
-- Single-record lookup
------------------------------------------------------------

Harness.section("GET one employee")

if #list == 0 then

    Harness.skip("get_employee", "no card code to ask about")

else

    local sample = list[1].card_code
    local record, lookupError = Server.get_employee(sample)

    if record == nil then
        -- Optional in the plan's flow: the daily list is what matters.
        Harness.skip("get_employee(" .. tostring(sample) .. ")", lookupError)
    else
        Harness.check("get_employee returns the same card", record.card_code == sample,
                      "got " .. tostring(record.card_code))
    end

end

------------------------------------------------------------
-- Error handling
------------------------------------------------------------

Harness.section("Error handling")

local missing, missingError = Server.request("/this-path-does-not-exist-smartlocker-test")

Harness.check("a 404 is reported as an error, not as data", missing == nil, missingError)

Harness.info("")
Harness.info("Checklist for Plan section 23:")
Harness.info("  [ ] authentication works (no 401)")
Harness.info("  [ ] every field the plan needs is present: username, card_code, role, gender, expire_at, active")
Harness.info("  [ ] a timeout or a 500 leaves the gateway serving from its local database")

Harness.finish()
