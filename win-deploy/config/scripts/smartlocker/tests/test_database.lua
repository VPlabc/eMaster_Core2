-- tests/test_database.lua
-- Stage 2.2 (Plan section 25): the local SQLite store.
--
-- Runs entirely against test_smartlocker.db -- never the live database. No PLC,
-- no reader, no server: this one can be run anywhere the Db.* binding exists.

local ok = pcall(require, "bootstrap")
if not ok then require("smartlocker.tests.bootstrap") end

local Database   = require("database.database")
local EmployeeDb = require("database.employee_db")
local Harness    = require("tests.harness")
local LockerDb   = require("database.locker_db")

Harness.start("Stage 2.2 -- database (SQLite)")

------------------------------------------------------------
-- The binding itself
------------------------------------------------------------

Harness.section("Binding")

if not Harness.check("Db.* is available in this build", type(Db) == "table" and type(Db.Open) == "function") then
    Harness.info("This gateway was built without the SQL binding. Nothing below can run.")
    Harness.finish()
    return
end

------------------------------------------------------------
-- Open and schema
------------------------------------------------------------

Harness.section("Open")

local opened, err = Database.open(Harness.TEST_DATABASE)

if not Harness.check("database opened", opened, err) then
    Harness.finish()
    return
end

Harness.check("path reported", Database.path() ~= nil, Database.path())

Database.reset()

Harness.check("tables are empty", #EmployeeDb.all() == 0 and #LockerDb.all() == 0)

-- The schema is what the web UI queries against, so its shape is worth
-- asserting rather than assuming.
for _, table_name in ipairs({ "employees", "lockers", "locker_logs", "system_logs", "meta" }) do
    local exists = Database.scalar(
        "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = ?", { table_name })
    Harness.check("table " .. table_name .. " exists", (exists or 0) > 0)
end

Harness.check("schema version recorded",
              Database.scalar("SELECT value FROM meta WHERE key = 'schema_version'") ~= nil)

------------------------------------------------------------
-- Employees: insert / query / update / delete
------------------------------------------------------------

Harness.section("Employees")

local alice = EmployeeDb.insert({
    username = "NGUYEN VAN A",
    card_code = "abcd0123",          -- deliberately lower case
    role = "employee",
    gender = "female",
    active = true,
})

Harness.check("insert returns a row with an id", alice ~= nil and alice.id ~= nil)
Harness.check("card codes are normalised to upper case", alice.card_code == "ABCD0123", alice.card_code)
Harness.check("created_at is stamped", alice.created_at ~= nil)
Harness.check("active comes back as a boolean, not 1", alice.active == true)

local bob = EmployeeDb.insert({
    username = "NGUYEN VAN B",
    card_code = "ABCD5678",
    role = "contractor",
    gender = "male",
    expire_at = "2026-12-31",
    active = true,
})

Harness.check("second insert gets a different id", bob.id ~= alice.id)
Harness.check("count is 2", EmployeeDb.count() == 2)

Harness.check("find by card, exact", EmployeeDb.find_by_card("ABCD0123") ~= nil)
Harness.check("find by card, different case", EmployeeDb.find_by_card("abcd0123") ~= nil)
Harness.check("find by card, unknown code returns nil", EmployeeDb.find_by_card("NOPE") == nil)

-- The UNIQUE constraint is what stops a duplicate card from ever existing,
-- whatever the business layer does.
local duplicate, duplicateErr = EmployeeDb.insert({ card_code = "ABCD0123", role = "employee" })

Harness.check("a duplicate card_code is rejected by the schema", duplicate == nil, duplicateErr)

EmployeeDb.update(alice.id, { username = "NGUYEN VAN AA" })

Harness.check("update applied", EmployeeDb.find_by_card("ABCD0123").username == "NGUYEN VAN AA")

EmployeeDb.set_active(bob.id, false)

Harness.check("set_active applied", EmployeeDb.find_by_card("ABCD5678").active == false)
Harness.check("active() lists only active rows", #EmployeeDb.active() == 1)

EmployeeDb.update(bob.id, { expire_at = Database.NULL })

Harness.check("Database.NULL clears a column", EmployeeDb.find_by_card("ABCD5678").expire_at == nil)

Harness.check("missing_from finds what a sync did not mention",
              #EmployeeDb.missing_from({ "ABCD0123" }) == 1)

------------------------------------------------------------
-- Lockers
------------------------------------------------------------

Harness.section("Lockers")

local layout = {}

for number = 1, 6 do
    layout[#layout + 1] = {
        block_id = 1, block_name = "Employee", locker_number = number, locker_type = "employee",
        input_register = 30001, input_bit = 7 + number,
        output_register = (number <= 4) and 40001 or nil,   -- two doors with no output on purpose
        output_bit = (number <= 4) and (7 + number) or nil,
    }
end

local added, updated, removed = LockerDb.ensure_layout(layout)

Harness.check("layout created six lockers", added == 6 and #LockerDb.all() == 6,
              string.format("added=%d updated=%d removed=%d", added, updated, removed))

-- Idempotence is what makes it safe to run at every start-up.
local added2, updated2 = LockerDb.ensure_layout(layout)

Harness.check("re-applying the layout changes nothing", added2 == 0 and updated2 == 0)

Harness.check("available() skips lockers with no output", #LockerDb.available("employee") == 4,
              #LockerDb.available("employee") .. " available of 6")

Harness.check("available() is sorted by number", LockerDb.available("employee")[1].locker_number == 1)

local locker = LockerDb.available("employee")[1]

LockerDb.assign(locker.id, { card_code = "ABCD0123", expire_at = nil }, "2026-08-14 09:00:00")

Harness.check("assign sets the status", LockerDb.get(locker.id).status == LockerDb.STATUS.ASSIGNED)
Harness.check("assign records the card", LockerDb.get_by_card("ABCD0123") ~= nil)
Harness.check("an assigned locker is no longer available", #LockerDb.available("employee") == 3)

LockerDb.set_door(locker.id, true)
LockerDb.set_runtime_state(locker.id, "WAIT_CLOSE")

Harness.check("door state mirrored for the UI", LockerDb.get(locker.id).door_open == true)
Harness.check("runtime state mirrored for the UI", LockerDb.get(locker.id).runtime_state == "WAIT_CLOSE")

LockerDb.set_door(locker.id, false)
LockerDb.set_runtime_state(locker.id, "IDLE")

LockerDb.set_error(locker.id, "DOOR_CLOSE_TIMEOUT")

Harness.check("error state stored", LockerDb.get(locker.id).status == LockerDb.STATUS.ERROR)
Harness.check("error reason stored", LockerDb.get(locker.id).last_error == "DOOR_CLOSE_TIMEOUT")

LockerDb.release(locker.id)

Harness.check("release empties the locker", LockerDb.get(locker.id).status == LockerDb.STATUS.EMPTY)
Harness.check("release clears the card", LockerDb.get(locker.id).card_code == nil)

local summary = LockerDb.summary()

Harness.check("summary counts everything", summary.total == 6 and summary.no_output == 2,
              "total=" .. summary.total .. " no_output=" .. summary.no_output)

------------------------------------------------------------
-- Audit trail
------------------------------------------------------------

Harness.section("Audit trail")

LockerDb.log({ locker_id = locker.id, card_code = "ABCD0123", event = "ACCESS_GRANTED", result = "GRANTED" })
LockerDb.log({ locker_id = locker.id, card_code = "ABCD0123", event = "DOOR_OPEN" })
LockerDb.log({ card_code = "NOSUCH", event = "CARD_NOT_FOUND", result = "DENIED", reason = "unknown card" })

Harness.check("rows are stored", #LockerDb.logs(nil, 10) == 3)
Harness.check("per-locker filter works", #LockerDb.logs(locker.id, 10) == 2)
Harness.check("newest first", LockerDb.logs(nil, 1)[1].event == "CARD_NOT_FOUND")

Harness.check("system_logs is writable",
              Database.insert("system_logs", { level = "INFO", type = "TEST", message = "hello" }) ~= nil)

------------------------------------------------------------
-- Transactions
------------------------------------------------------------

Harness.section("Transactions")

local before = EmployeeDb.count()

Database.begin()
EmployeeDb.insert({ username = "ROLLBACK ME", card_code = "DEAD0001", role = "employee", active = true })
Harness.check("row is visible inside the transaction", EmployeeDb.find_by_card("DEAD0001") ~= nil)
Database.rollback()

Harness.check("rollback removed it", EmployeeDb.find_by_card("DEAD0001") == nil)
Harness.check("count is back to what it was", EmployeeDb.count() == before)

Database.begin()
EmployeeDb.insert({ username = "COMMIT ME", card_code = "C0DE0001", role = "employee", active = true })
Database.commit()

Harness.check("commit kept it", EmployeeDb.find_by_card("C0DE0001") ~= nil)

-- transaction_do rolls back when the body raises -- the guarantee assignment
-- and release depend on.
local okDo, errDo = Database.transaction_do(function()
    EmployeeDb.insert({ username = "BOOM", card_code = "B00M0001", role = "employee", active = true })
    error("something went wrong halfway")
end)

Harness.check("transaction_do reports the failure", okDo == false, errDo)
Harness.check("transaction_do rolled the whole thing back", EmployeeDb.find_by_card("B00M0001") == nil)

local okBody = Database.transaction_do(function()
    EmployeeDb.insert({ username = "GOOD", card_code = "600D0001", role = "employee", active = true })
    return true
end)

Harness.check("transaction_do commits on success", okBody and EmployeeDb.find_by_card("600D0001") ~= nil)

-- Nested: an inner commit must not end the outer transaction.
Database.begin()
Database.begin()
EmployeeDb.insert({ username = "NESTED", card_code = "4E570001", role = "employee", active = true })
Database.commit()   -- inner
Harness.check("still inside the outer transaction", Database.in_transaction())
Database.rollback() -- outer
Harness.check("outer rollback undid the nested insert", EmployeeDb.find_by_card("4E570001") == nil)

------------------------------------------------------------
-- Persistence
------------------------------------------------------------

Harness.section("Persistence")

local reopened = Database.open(Harness.TEST_DATABASE)

Harness.check("re-opened", reopened)
Harness.check("employees survived the round trip", EmployeeDb.find_by_card("ABCD0123") ~= nil)
Harness.check("lockers survived the round trip", #LockerDb.all() == 6)

local nextRow = EmployeeDb.insert({ card_code = "FEED0001", role = "employee" })

Harness.check("ids did not restart", nextRow.id > bob.id)

------------------------------------------------------------
-- Errors and delete
------------------------------------------------------------

Harness.section("Errors")

local bad, badErr = Database.query("SELECT * FROM there_is_no_such_table")

Harness.check("a bad statement returns nil plus a message", bad == nil, badErr)

local mismatch, mismatchErr = Db.Query("SELECT * FROM employees WHERE id = ? AND card_code = ?", { 1 })

Harness.check("a wrong parameter count is refused", mismatch == nil, mismatchErr)

-- Injection attempt through a bound parameter: it must be treated as data.
EmployeeDb.insert({ card_code = "X'; DROP TABLE employees; --", role = "employee" })

Harness.check("employees table is still there after a quoted parameter",
              Database.count("employees") > 0)

Harness.section("Delete")

Harness.check("remove by card code", EmployeeDb.remove("C0DE0001") == true)
Harness.check("the row is gone", EmployeeDb.find_by_card("C0DE0001") == nil)
Harness.check("removing an unknown card is not an error", EmployeeDb.remove("NOPE") == false)

Harness.info("")
Harness.info("Database file: " .. tostring(Database.path()))
Harness.info("It is emptied at the start of each run; delete the file to start from nothing.")

Harness.finish()
