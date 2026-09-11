-- tests/test_contractor_usage.lua
-- CardScanPlan section 1: the contractor working day.
--
-- Runs with no hardware at all -- database and clock arithmetic only. Every
-- function under test takes the moment as an argument, so the whole day (and
-- the day after it) is exercised without waiting for one, and the test gives
-- the same answer at 03:00 as at 18:00. Nothing here calls os.time() for a
-- decision; a test that did would pass in the morning and fail after lunch.

local ok = pcall(require, "bootstrap")
if not ok then require("smartlocker.tests.bootstrap") end

local Config     = require("config")
local Database   = require("database.database")
local Employee   = require("core.employee")
local EmployeeDb = require("database.employee_db")
local Harness    = require("tests.harness")
local Time       = require("utils.time")
local Usage      = require("core.contractor_usage")

Harness.start("CardScanPlan 1 -- contractor daily usage")

local opened, openError = Harness.fresh_database()

if not Harness.check("test database", opened, openError) then
    Harness.finish()
    return
end

------------------------------------------------------------
-- The settings this test assumes, whatever the site file says
------------------------------------------------------------

-- Restored at the end: Config is a live singleton and a later test in the same
-- run must not inherit an 08:00 morning it never asked for.
local savedContractor = Config.contractor

Config.contractor = {
    morning_start = "08:00",
    morning_end = "09:30",
    afternoon_start = "17:00",
    afternoon_end = "19:00",
    allow_ot = true,
    usage_timeout_enabled = false,
    enforce_start_date = true,
    overtime_expires_daily = true,
    complete_scan_count = 3,
    complete_scan_timeout = 10,
}

------------------------------------------------------------
-- Fixtures
------------------------------------------------------------

local TODAY = "2026-08-18"
local YESTERDAY = "2026-08-17"

local function at(dateText, hour, minute)

    local year, month, day = dateText:match("^(%d%d%d%d)%-(%d%d)%-(%d%d)$")

    return os.time({
        year = tonumber(year), month = tonumber(month), day = tonumber(day),
        hour = hour, min = minute, sec = 0, isdst = false,
    })

end

local function contractor(code, fields)

    local record = {
        username = "Contractor " .. code,
        card_code = code,
        role = "contractor",
        expire_at = "2026-12-31",
    }

    for key, value in pairs(fields or {}) do
        record[key] = value
    end

    local row = EmployeeDb.insert(record)

    return row

end

------------------------------------------------------------
-- The database carries the day
------------------------------------------------------------

Harness.section("Storage")

local stored = contractor("USE00001", { start_at = "2026-08-01" })

Harness.check("a contractor row is stored", stored ~= nil)

if stored == nil then
    Harness.finish()
    Config.contractor = savedContractor
    return
end

Harness.check("start_at survives the round trip", stored.start_at == "2026-08-01", stored.start_at)
Harness.check("a new card is NOT_STARTED", stored.usage_status == "NOT_STARTED", stored.usage_status)
Harness.check("with no usage day yet", stored.usage_date == nil)

------------------------------------------------------------
-- Windows
------------------------------------------------------------

Harness.section("The working day")

local windowCases = {
    { 7, 0,  "before" },
    { 8, 0,  "morning" },
    { 9, 30, "morning" },
    { 9, 31, "midday" },
    { 16, 59, "midday" },
    { 17, 0, "afternoon" },
    { 19, 0, "afternoon" },
    { 19, 1, "after" },
    { 23, 0, "after" },
}

for _, case in ipairs(windowCases) do

    local actual = Usage.window(at(TODAY, case[1], case[2]))

    Harness.check(string.format("%02d:%02d is %s", case[1], case[2], case[3]),
                  actual == case[3], actual)

end

------------------------------------------------------------
-- Using the locker never finishes the day
------------------------------------------------------------

Harness.section("Opens are just opens")

Usage.reset_sequence()

local row, status = Usage.on_locker_open(stored, nil, at(TODAY, 8, 15))

Harness.check("the first open starts the day", status == "USING", status)
Harness.check("usage_date is today", row.usage_date == TODAY, row.usage_date)
Harness.check("usage_started_at is stamped", row.usage_started_at ~= nil)
Harness.check("nothing is completed yet", row.usage_completed_at == nil)

row, status = Usage.on_locker_open(row, nil, at(TODAY, 8, 45))

Harness.check("a second open stays USING", status == "USING", status)
Harness.check("and does not move the start", row.usage_started_at:find("08:15", 1, true) ~= nil,
              row.usage_started_at)

-- The clock finishes nothing any more. These two are the assertions that would
-- have failed under the old "afternoon completes the day" rule, and they are
-- the point of the change: opening a locker at 17:30 is opening a locker.
row, status = Usage.on_locker_open(row, nil, at(TODAY, 17, 30))

Harness.check("an evening open is still only USING", status == "USING", status)
Harness.check("with no completion stamp", row.usage_completed_at == nil)

row, status = Usage.on_locker_open(row, nil, at(TODAY, 20, 30))

Harness.check("an open past afternoon_end is still USING", status == "USING", status)

-- Four opens, spread out. Each is more than complete_scan_timeout after the
-- last, so the sequence restarts every time and nothing adds up.
Harness.check("opens spread across the day never finish it",
              EmployeeDb.find_by_card("USE00001").usage_status == "USING",
              EmployeeDb.find_by_card("USE00001").usage_status)

------------------------------------------------------------
-- The finishing gesture
------------------------------------------------------------

Harness.section("Three scans in a row")

local worker = contractor("USE00002")

Usage.reset_sequence()

row, status = Usage.on_locker_open(worker, nil, at(TODAY, 9, 0))
Harness.check("scan 1 -> USING", status == "USING", status)

row, status = Usage.on_locker_open(row, nil, at(TODAY, 9, 0) + 4)
Harness.check("scan 2 (4s later) still USING", status == "USING", status)

local changed
row, status, changed = Usage.on_locker_open(row, nil, at(TODAY, 9, 0) + 8)

Harness.check("scan 3 finishes the day", status == "COMPLETED", status)
Harness.check("and reports the transition", changed == true)
Harness.check("usage_completed_at is stamped", row.usage_completed_at ~= nil)
Harness.check("the day still starts where it started",
              row.usage_started_at:find("09:00", 1, true) ~= nil, row.usage_started_at)

-- "if complete but use time not ending, use again after scan card status change
-- to using": they came back, and their period has not ended.
row, status, changed = Usage.on_locker_open(row, nil, at(TODAY, 9, 30))

Harness.check("an open after finishing reopens the day", status == "USING", status)
Harness.check("reported as a change", changed == true)
Harness.check("the completion stamp was cleared", row.usage_completed_at == nil)
Harness.check("the start time is untouched",
              row.usage_started_at:find("09:00", 1, true) ~= nil, row.usage_started_at)
Harness.check("and they are never refused for it",
              Usage.check_daily_usage(row, at(TODAY, 9, 30)) == true)

-- "complete with scan 3 times" -- again.
Usage.on_locker_open(EmployeeDb.find_by_card("USE00002"), nil, at(TODAY, 17, 0))
Usage.on_locker_open(EmployeeDb.find_by_card("USE00002"), nil, at(TODAY, 17, 0) + 4)
row, status = Usage.on_locker_open(EmployeeDb.find_by_card("USE00002"), nil, at(TODAY, 17, 0) + 8)

Harness.check("three more scans finish it again", status == "COMPLETED", status)

------------------------------------------------------------
-- What breaks the sequence
------------------------------------------------------------

Harness.section("Breaking the sequence")

local slow = contractor("USE00003")

Usage.reset_sequence()
Usage.on_locker_open(slow, nil, at(TODAY, 10, 0))
Usage.on_locker_open(EmployeeDb.find_by_card("USE00003"), nil, at(TODAY, 10, 0) + 4)
row, status = Usage.on_locker_open(EmployeeDb.find_by_card("USE00003"), nil, at(TODAY, 10, 0) + 40)

Harness.check("a gap longer than the timeout restarts the count", status == "USING", status)

local other = contractor("USE00009")

Usage.reset_sequence()
Usage.on_locker_open(EmployeeDb.find_by_card("USE00003"), nil, at(TODAY, 11, 0))
Usage.on_locker_open(EmployeeDb.find_by_card("USE00003"), nil, at(TODAY, 11, 0) + 4)
Usage.on_locker_open(other, nil, at(TODAY, 11, 0) + 6)
row, status = Usage.on_locker_open(EmployeeDb.find_by_card("USE00003"), nil, at(TODAY, 11, 0) + 8)

Harness.check("another card interrupting restarts the count", status == "USING", status)

-- complete_scan_timeout = 0 turns the gesture into "any N opens today".
Config.contractor.complete_scan_timeout = 0

local slowGesture = contractor("USE00010")

Usage.reset_sequence()
Usage.on_locker_open(slowGesture, nil, at(TODAY, 8, 0))
Usage.on_locker_open(EmployeeDb.find_by_card("USE00010"), nil, at(TODAY, 12, 0))
row, status = Usage.on_locker_open(EmployeeDb.find_by_card("USE00010"), nil, at(TODAY, 16, 0))

Harness.check("with no timeout, three opens across the day finish it",
              status == "COMPLETED", status)

Config.contractor.complete_scan_timeout = 10
Usage.reset_sequence()

------------------------------------------------------------
-- The day that was never closed
------------------------------------------------------------

Harness.section("End of day")

local unclosed = contractor("USE00004")
Usage.on_locker_open(unclosed, nil, at(YESTERDAY, 8, 20))

Harness.check("yesterday is USING",
              EmployeeDb.find_by_card("USE00004").usage_status == "USING")

local closed = Usage.end_of_day(at(TODAY, 0, 30))

Harness.check("end_of_day closes it", closed >= 1, closed)
Harness.check("as EXPIRED",
              EmployeeDb.find_by_card("USE00004").usage_status == "EXPIRED",
              EmployeeDb.find_by_card("USE00004").usage_status)
Harness.check("and running it again changes nothing",
              Usage.end_of_day(at(TODAY, 1, 0)) == 0)

-- The point of the whole design: yesterday's EXPIRED day is not a refusal today.
local reopened = EmployeeDb.find_by_card("USE00004")

Harness.check("yesterday's EXPIRED day reads NOT_STARTED today",
              Usage.status(reopened, at(TODAY, 8, 0)) == "NOT_STARTED",
              Usage.status(reopened, at(TODAY, 8, 0)))
Harness.check("and the card is allowed",
              Usage.check_daily_usage(reopened, at(TODAY, 8, 0)) == true)

row, status = Usage.on_locker_open(reopened, nil, at(TODAY, 8, 5))

Harness.check("a new day starts clean", status == "USING", status)
Harness.check("with the old completion stamp cleared", row.usage_completed_at == nil)

-- A day that finished properly is left exactly as it was. Finished the only
-- way a day can be finished now: the scan sequence.
local finished = contractor("USE00005")

Usage.reset_sequence()
Usage.on_locker_open(finished, nil, at(YESTERDAY, 17, 30))
Usage.on_locker_open(EmployeeDb.find_by_card("USE00005"), nil, at(YESTERDAY, 17, 30) + 4)
Usage.on_locker_open(EmployeeDb.find_by_card("USE00005"), nil, at(YESTERDAY, 17, 30) + 8)

Harness.check("yesterday was finished with the gesture",
              EmployeeDb.find_by_card("USE00005").usage_status == "COMPLETED",
              EmployeeDb.find_by_card("USE00005").usage_status)

Usage.end_of_day(at(TODAY, 2, 0))

Harness.check("a COMPLETED day is not rewritten",
              EmployeeDb.find_by_card("USE00005").usage_status == "COMPLETED",
              EmployeeDb.find_by_card("USE00005").usage_status)

------------------------------------------------------------
-- The registered period
------------------------------------------------------------

Harness.section("Period")

local lapsed = contractor("USE00006", { expire_at = "2026-08-17" })

Harness.check("a card past its expiry is expired",
              Usage.is_expired(lapsed, at(TODAY, 8, 0)) == true)
Harness.check("and reads EXPIRED whatever its last day said",
              Usage.status(lapsed, at(TODAY, 8, 0)) == "EXPIRED",
              Usage.status(lapsed, at(TODAY, 8, 0)))

local future = contractor("USE00007", { start_at = "2026-08-20" })

local allowed, reason, event = Usage.check_daily_usage(future, at(TODAY, 8, 0))

Harness.check("a card before its start date is refused", allowed == false, reason)
Harness.check("with USAGE_NOT_STARTED", event == "USAGE_NOT_STARTED", event)
Harness.check("on the start date it is allowed",
              Usage.check_daily_usage(future, at("2026-08-20", 8, 0)) == true)

Config.contractor.enforce_start_date = false
Harness.check("enforce_start_date = false lets it through",
              Usage.check_daily_usage(future, at(TODAY, 8, 0)) == true)
Config.contractor.enforce_start_date = true

local open = contractor("USE00008")
Harness.check("a card with no start_at has started",
              Usage.has_started(open, at(TODAY, 8, 0)) == true)

------------------------------------------------------------
-- The optional hard gate
------------------------------------------------------------

Harness.section("usage_timeout_enabled")

Harness.check("with the gate off, any hour is allowed",
              Usage.check_daily_usage(open, at(TODAY, 13, 0)) == true)

Config.contractor.usage_timeout_enabled = true

allowed, reason, event = Usage.check_daily_usage(open, at(TODAY, 13, 0))

Harness.check("with the gate on, midday is refused", allowed == false, reason)
Harness.check("with USAGE_OUT_OF_HOURS", event == "USAGE_OUT_OF_HOURS", event)
Harness.check("06:00 is refused", Usage.check_daily_usage(open, at(TODAY, 6, 0)) == false)
Harness.check("08:30 is allowed", Usage.check_daily_usage(open, at(TODAY, 8, 30)) == true)
Harness.check("17:30 is allowed", Usage.check_daily_usage(open, at(TODAY, 17, 30)) == true)
Harness.check("20:00 is allowed while allow_ot is on",
              Usage.check_daily_usage(open, at(TODAY, 20, 0)) == true)

Config.contractor.allow_ot = false
Harness.check("20:00 is refused with allow_ot off",
              Usage.check_daily_usage(open, at(TODAY, 20, 0)) == false)

------------------------------------------------------------
-- Overtime pushed in by the server
------------------------------------------------------------

Harness.section("Overtime")

local applied, overtimeError = Usage.set_overtime("21:00", at(TODAY, 18, 0))

Harness.check("overtime is accepted", applied == true, overtimeError)
Harness.check("and reported", Usage.overtime(at(TODAY, 18, 0)) == "21:00",
              Usage.overtime(at(TODAY, 18, 0)))
Harness.check("20:00 is now inside the afternoon window",
              Usage.window(at(TODAY, 20, 0)) == "afternoon",
              Usage.window(at(TODAY, 20, 0)))
Harness.check("so it is allowed even with allow_ot off",
              Usage.check_daily_usage(open, at(TODAY, 20, 0)) == true)
Harness.check("21:30 is still past it", Usage.window(at(TODAY, 21, 30)) == "after")

-- The rule that matters: tonight's overtime is not tomorrow's finishing time.
Harness.check("overtime does not carry into the next day",
              Usage.overtime(at("2026-08-19", 18, 0)) == nil)

Harness.check("a value that is not a time is refused",
              Usage.set_overtime("this evening") == false)

-- An overtime BEFORE afternoon_start would otherwise empty the window.
Usage.set_overtime("02:00", at(TODAY, 18, 0))
Harness.check("a nonsense overtime leaves the window usable",
              Usage.window(at(TODAY, 18, 0)) == "afternoon",
              Usage.window(at(TODAY, 18, 0)))

Harness.check("overtime can be cleared", Usage.set_overtime(nil) == true)
Harness.check("and is then gone", Usage.overtime(at(TODAY, 18, 0)) == nil)

Config.contractor.allow_ot = true
Config.contractor.usage_timeout_enabled = false

------------------------------------------------------------
-- Employees are not contractors
------------------------------------------------------------

Harness.section("Employees")

local staff = EmployeeDb.insert({
    username = "Staff",
    card_code = "EMP00001",
    role = "employee",
})

Harness.check("an employee has no daily status", Usage.status(staff) == nil)
Harness.check("an employee is never gated",
              Usage.check_daily_usage(staff, at(TODAY, 3, 0)) == true)

local untouched = Usage.on_locker_open(staff, nil, at(TODAY, 8, 0))

Harness.check("and their row is not written", untouched.usage_date == nil)
Harness.check("not even in the database",
              EmployeeDb.find_by_card("EMP00001").usage_date == nil)

------------------------------------------------------------
-- Auto-reclaim: finished AND expired, never one alone
------------------------------------------------------------

Harness.section("Reclaiming a finished contractor's locker")

-- Written straight into the row rather than played through the state machine:
-- what is under test is the RULE, and a fixture that has to act out three days
-- to reach a state tests the acting more than the rule.
local function usageRow(code, expireAt, usageDate, usageStatus)

    local row = contractor(code, { expire_at = expireAt })

    return EmployeeDb.update(row.id, {
        usage_date = usageDate,
        usage_status = usageStatus,
    })

end

local finishedAndDone = usageRow("USE00020", "2026-08-17", "2026-08-17", "COMPLETED")

Harness.check("completed + expired is reclaimable",
              Usage.is_reclaimable(finishedAndDone, at(TODAY, 9, 0)) == true)

-- The case that makes this safe to run unattended: their period ended, but
-- nobody ever said the locker was empty.
local abandoned = usageRow("USE00021", "2026-08-17", "2026-08-17", "EXPIRED")
local canTake, why = Usage.is_reclaimable(abandoned, at(TODAY, 9, 0))

Harness.check("expired but NEVER finished is not reclaimable", canTake == false, why)

local caughtMidDay = usageRow("USE00022", "2026-08-17", "2026-08-17", "USING")

Harness.check("expired mid-day is not reclaimable",
              Usage.is_reclaimable(caughtMidDay, at(TODAY, 9, 0)) == false)

-- Finished today but still within their period: they are back tomorrow.
local stillWorking = usageRow("USE00023", "2026-12-31", TODAY, "COMPLETED")

Harness.check("finished but still in period is not reclaimable",
              Usage.is_reclaimable(stillWorking, at(TODAY, 18, 0)) == false)

local staff = EmployeeDb.insert({
    username = "Staff", card_code = "USE00024", role = "employee", expire_at = "2026-08-17",
})

Harness.check("an employee is never reclaimable",
              Usage.is_reclaimable(staff, at(TODAY, 9, 0)) == false)

local list = Usage.reclaimable(at(TODAY, 9, 0))

Harness.check("exactly one card qualifies", #list == 1, #list)
Harness.check("and it is the finished, expired one",
              list[1] and list[1].employee.card_code == "USE00020",
              list[1] and list[1].employee.card_code)
Harness.check("the reason names both halves",
              list[1] and list[1].reason:find("completed", 1, true) ~= nil,
              list[1] and list[1].reason)

------------------------------------------------------------
-- Time parsing
------------------------------------------------------------

Harness.section("HH:MM parsing")

Harness.check("08:00", Usage.minutes_of("08:00") == 480)
Harness.check("8:00", Usage.minutes_of("8:00") == 480)
Harness.check("0930", Usage.minutes_of("0930") == 570)
Harness.check("25:00 is rejected", Usage.minutes_of("25:00") == nil)
Harness.check("08:70 is rejected", Usage.minutes_of("08:70") == nil)
Harness.check("'evening' is rejected", Usage.minutes_of("evening") == nil)
Harness.check("570 prints as 09:30", Usage.clock_of(570) == "09:30", Usage.clock_of(570))

Config.contractor = savedContractor

Harness.finish()
