-- tests/test_employee.lua
-- Stage 4 (Plan section 27): employee and contractor records.
--
-- Pure logic against the test database -- no PLC, no reader, no server. This is
-- the test to run after touching anything in core/employee.lua.

local ok = pcall(require, "bootstrap")
if not ok then require("smartlocker.tests.bootstrap") end

local Config     = require("config")
local Database   = require("database.database")
local Employee   = require("core.employee")
local EmployeeDb = require("database.employee_db")
local Harness    = require("tests.harness")
local Time       = require("utils.time")

Harness.start("Stage 4 -- employees and contractors")

local opened, err = Harness.fresh_database()

if not Harness.check("test database", opened, err) then
    Harness.finish()
    return
end

------------------------------------------------------------
-- Role and gender normalisation
------------------------------------------------------------

Harness.section("Role normalisation")

local roleCases = {
    { "employee",            Employee.ROLE.EMPLOYEE },
    { "Employee",            Employee.ROLE.EMPLOYEE },
    { "STAFF",               Employee.ROLE.EMPLOYEE },
    { "Nhan vien",           Employee.ROLE.EMPLOYEE },
    { "nhân viên",           Employee.ROLE.EMPLOYEE },
    { "contractor",          Employee.ROLE.CONTRACTOR },
    { "External Contractor", Employee.ROLE.CONTRACTOR },
    { "NHA THAU",            Employee.ROLE.CONTRACTOR },
    { "nhà thầu",            Employee.ROLE.CONTRACTOR },
    { "vendor",              Employee.ROLE.CONTRACTOR },
    { "something else",      nil },
}

for _, case in ipairs(roleCases) do
    Harness.check("role '" .. case[1] .. "'", Employee.normalize_role(case[1]) == case[2],
                  "-> " .. tostring(Employee.normalize_role(case[1])))
end

Harness.section("Gender normalisation")

local genderCases = {
    { "female", Employee.GENDER.FEMALE },
    { "FEMALE", Employee.GENDER.FEMALE },
    { "F",      Employee.GENDER.FEMALE },
    { "Nu",     Employee.GENDER.FEMALE },
    { "nữ",     Employee.GENDER.FEMALE },
    { "male",   Employee.GENDER.MALE },
    { "M",      Employee.GENDER.MALE },
    { "Nam",    Employee.GENDER.MALE },
    { "",       nil },
}

for _, case in ipairs(genderCases) do
    Harness.check("gender '" .. case[1] .. "'", Employee.normalize_gender(case[1]) == case[2],
                  "-> " .. tostring(Employee.normalize_gender(case[1])))
end

------------------------------------------------------------
-- Validation
------------------------------------------------------------

Harness.section("Validation")

-- The two examples from Plan section 2.
local employeeRecord = {
    username = "NGUYEN VAN A",
    card_code = "ABCD0123",
    role = "employee",
    gender = "female",
    expire_at = nil,
    active = true,
}

local contractorRecord = {
    username = "NGUYEN VAN B",
    card_code = "ABCD5678",
    role = "contractor",
    gender = "male",
    expire_at = "2026-12-31",
    active = true,
}

local validEmployee = Employee.validate(employeeRecord)
local validContractor = Employee.validate(contractorRecord)

Harness.check("the section 2 employee validates", validEmployee ~= nil)
Harness.check("  role is employee", validEmployee and validEmployee.role == Employee.ROLE.EMPLOYEE)
Harness.check("  no expiry", validEmployee and validEmployee.expire_at == nil)

Harness.check("the section 2 contractor validates", validContractor ~= nil)
Harness.check("  role is contractor", validContractor and validContractor.role == Employee.ROLE.CONTRACTOR)
Harness.check("  expiry kept", validContractor and validContractor.expire_at == "2026-12-31")

local noCard, noCardReason = Employee.validate({ username = "NO CARD", role = "employee" })

Harness.check("a record with no card_code is rejected", noCard == nil, noCardReason)

local badDate, badDateReason = Employee.validate({ card_code = "X1", role = "contractor", expire_at = "next tuesday" })

Harness.check("an unparseable expire_at is rejected", badDate == nil, badDateReason)

local unknownRole = Employee.validate({ card_code = "X2", role = "wizard" })

Harness.check("an unrecognised role defaults to employee (and warns)",
              unknownRole ~= nil and unknownRole.role == Employee.ROLE.EMPLOYEE)

-- CardScanPlan section 1's contractor_start_date. It is optional, and unlike
-- expire_at an unreadable one costs the record only the "not before" check
-- rather than the whole record.
local withStart = Employee.validate({
    card_code = "X3",
    role = "contractor",
    expire_at = "2026-12-31",
    start_at = "2026-08-01",
})

Harness.check("a start_at is kept", withStart ~= nil and withStart.start_at == "2026-08-01",
              withStart and withStart.start_at)

local badStart = Employee.validate({
    card_code = "X4",
    role = "contractor",
    expire_at = "2026-12-31",
    start_at = "next monday",
})

Harness.check("an unparseable start_at does NOT reject the record", badStart ~= nil)
Harness.check("  it is dropped instead (and warns)", badStart and badStart.start_at == nil)

------------------------------------------------------------
-- Expiry (Plan section 10)
------------------------------------------------------------

Harness.section("Expiry")

local yesterday = os.date("%Y-%m-%d", os.time() - 86400)
local tomorrow = os.date("%Y-%m-%d", os.time() + 86400)
local today = os.date("%Y-%m-%d")

Harness.check("no expire_at is never expired",
              Employee.is_expired({ role = "employee", expire_at = nil }) == false)

Harness.check("an empty expire_at is never expired",
              Employee.is_expired({ role = "employee", expire_at = "" }) == false)

Harness.check("yesterday is expired",
              Employee.is_expired({ role = "contractor", expire_at = yesterday }) == true)

Harness.check("tomorrow is not expired",
              Employee.is_expired({ role = "contractor", expire_at = tomorrow }) == false)

-- The rule that decides whether a contractor works on their last day.
Harness.check("today is still valid (a date-only expiry covers the whole day)",
              Employee.is_expired({ role = "contractor", expire_at = today }) == false,
              "sync.expire_at_end_of_day = " .. tostring(Config.sync.expire_at_end_of_day))

Harness.check("an ISO timestamp parses",
              Time.Parse("2026-12-31T23:59:59") ~= nil)

Harness.check("an epoch number parses",
              Time.Parse(1798761599) == 1798761599)

Harness.check("status of an expired contractor",
              Employee.status({ active = true, expire_at = yesterday }) == "EXPIRED")

Harness.check("status of an inactive card",
              Employee.status({ active = false }) == "INACTIVE")

Harness.check("status of a normal employee",
              Employee.status({ active = true }) == "ACTIVE")

------------------------------------------------------------
-- Upsert and duplicates
------------------------------------------------------------

Harness.section("Upsert")

local row, action = Employee.upsert(employeeRecord)

Harness.check("first upsert adds", action == "added" and row ~= nil)

local row2, action2 = Employee.upsert(employeeRecord)

Harness.check("the same record again is unchanged", action2 == "unchanged")
Harness.check("no duplicate row was created", EmployeeDb.count() == 1, EmployeeDb.count() .. " rows")
Harness.check("the id is stable", row2.id == row.id)

local renamed = {}

for key, value in pairs(employeeRecord) do
    renamed[key] = value
end

renamed.username = "NGUYEN VAN A (RENAMED)"

local row3, action3 = Employee.upsert(renamed)

Harness.check("a changed field updates", action3 == "updated" and row3.username == renamed.username)

local added, addError = Employee.add(employeeRecord)

Harness.check("add() refuses a duplicate card", added == nil, addError)

------------------------------------------------------------
-- Sync (Plan section 3.1)
------------------------------------------------------------

Harness.section("Sync")

Database.reset()

local serverList = {
    employeeRecord,
    contractorRecord,
    { username = "NGUYEN VAN C", card_code = "ABCD9999", role = "employee", gender = "male", active = true },
}

local stats = Employee.sync(serverList, { full_list = true })

Harness.check("three cards added", stats.added == 3, "added=" .. stats.added)
Harness.check("nothing invalid", stats.invalid == 0)

-- Second run with the same list: nothing should move.
local stats2 = Employee.sync(serverList, { full_list = true })

Harness.check("a repeat sync changes nothing",
              stats2.added == 0 and stats2.updated == 0 and stats2.deactivated == 0,
              string.format("added=%d updated=%d deactivated=%d", stats2.added, stats2.updated, stats2.deactivated))

-- One card withdrawn.
local shortList = { employeeRecord, contractorRecord }

local stats3, changes3 = Employee.sync(shortList, { full_list = true })

Harness.check("a withdrawn card is deactivated", stats3.deactivated == 1)
Harness.check("it is reported in changes", #changes3.deactivated == 1 and
              changes3.deactivated[1].card_code == "ABCD9999")
Harness.check("the row is kept, not deleted", EmployeeDb.find_by_card("ABCD9999") ~= nil)
Harness.check("and is now inactive", EmployeeDb.find_by_card("ABCD9999").active == false)

-- Without full_list (the RabbitMQ path), nothing is deactivated.
Employee.sync(serverList, { full_list = true })

local stats4 = Employee.sync({ employeeRecord }, { full_list = false })

Harness.check("a partial sync deactivates nothing", stats4.deactivated == 0)

-- A malformed record in an otherwise good list must not stop the rest.
local mixed = { employeeRecord, { username = "BROKEN" }, contractorRecord }

local stats5 = Employee.sync(mixed, { full_list = false })

Harness.check("a bad record is counted and skipped", stats5.invalid == 1)

Harness.finish()
