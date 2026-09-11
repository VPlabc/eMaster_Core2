-- tests/test_admin_access.lua
-- CardScanPlan sections 3-9: admin cards, the scan sequence and the override.
--
-- No hardware. Locker.unlock is replaced with a recorder, exactly as
-- test_locker.lua does it, so what is checked here is the DECISION -- which
-- door, how many of them, and in what order -- rather than whether a coil
-- moved. The wiring has its own tests.
--
-- Timeouts are driven by passing the moment in rather than by sleeping: the
-- override lasts a minute, and a test that actually waited one would be a test
-- nobody runs.

local ok = pcall(require, "bootstrap")
if not ok then require("smartlocker.tests.bootstrap") end

local Access     = require("core.access")
local Admin      = require("core.admin_access")
local Config     = require("config")
local Database   = require("database.database")
local Employee   = require("core.employee")
local EmployeeDb = require("database.employee_db")
local Harness    = require("tests.harness")
local Locker     = require("core.locker")
local LockerDb   = require("database.locker_db")
local Time       = require("utils.time")

Harness.start("CardScanPlan 3-9 -- admin access")

local opened, openError = Harness.fresh_database()

if not Harness.check("test database", opened, openError) then
    Harness.finish()
    return
end

------------------------------------------------------------
-- A cabinet, no PLC, and an admin list
------------------------------------------------------------

local layout = {}

for number = 1, 6 do
    layout[#layout + 1] = {
        block_id = 1,
        block_name = "Contractor",
        locker_number = number,
        locker_type = "contractor",
        input_register = 30001,
        input_bit = 7 + number,
        -- Locker 6 reports its state but cannot be opened, which is what the
        -- expired-locker walk has to skip rather than fail on.
        output_register = (number <= 5) and 40001 or nil,
        output_bit = (number <= 5) and (7 + number) or nil,
    }
end

LockerDb.ensure_layout(layout)

Harness.check("six lockers configured", #LockerDb.all() == 6)

local savedAdmin = Config.admin
local savedBlocks = Config.blocks

Config.admin = {
    enabled = true,
    cards = { "ADMIN0001", "admin0002" },
    unlock_all_scan_count = 5,
    scan_timeout = 10,
    override_timeout = 60,
    scan_opens_expired = true,
    override_expired = true,
}

Config.blocks = { { id = 1, name = "Contractor", type = "contractor", count = 6 } }

local unlocked = {}
local savedUnlock = Locker.unlock
local savedBusy = Locker.busy

Locker.unlock = function(lockerId, cardCode)
    unlocked[#unlocked + 1] = { locker_id = lockerId, card_code = cardCode }
    return true
end

Locker.busy = function() return false end

local function restore()
    Locker.unlock = savedUnlock
    Locker.busy = savedBusy
    Config.admin = savedAdmin
    Config.blocks = savedBlocks
    Admin.reset()
    Admin.reload()
end

Admin.init()

------------------------------------------------------------
-- Recognition (section 3)
------------------------------------------------------------

Harness.section("Recognising an admin card")

Harness.check("a listed card is an admin card", Admin.is_admin_card("ADMIN0001") == true)
Harness.check("case and spacing do not matter", Admin.is_admin_card("  admin0001 ") == true)
Harness.check("the second card counts too", Admin.is_admin_card("ADMIN0002") == true)
Harness.check("an ordinary card does not", Admin.is_admin_card("ABCD0123") == false)
Harness.check("nor does nil", Admin.is_admin_card(nil) == false)
Harness.check("both cards are counted", Admin.count() == 2, Admin.count())

-- The plan's rule: an admin card is not stored as an employee.
Harness.check("no admin card is in the employees table",
              EmployeeDb.find_by_card("ADMIN0001") == nil)

Config.admin.enabled = false
Admin.reload()
Harness.check("nothing is an admin card while admin.enabled is false",
              Admin.is_admin_card("ADMIN0001") == false)
Config.admin.enabled = true
Admin.reload()

------------------------------------------------------------
-- The scan sequence (section 7)
------------------------------------------------------------

Harness.section("The scan sequence")

Admin.reset()

Harness.check("it starts NORMAL", Admin.state() == "NORMAL", Admin.state())

local result = Admin.on_admin_scan("ADMIN0001")

Harness.check("one scan opens nothing", result.granted == false)
Harness.check("and the state names the count", Admin.state() == "ADMIN_SCAN_1", Admin.state())

for _ = 1, 3 do
    Admin.on_admin_scan("ADMIN0001")
end

Harness.check("four scans is still not enough", Admin.state() == "ADMIN_SCAN_4", Admin.state())
Harness.check("the override is not open", Admin.is_override_active() == false)

result = Admin.on_admin_scan("ADMIN0001")

Harness.check("the fifth opens it", Admin.is_override_active() == true, Admin.state())
Harness.check("and says so", result.event == "ADMIN_MODE_ENTER", result.event)

Admin.reset()

for _ = 1, 3 do
    Admin.on_admin_scan("ADMIN0001")
end

Admin.note_other_card("ABCD0123")
Harness.check("somebody else's card breaks the sequence", Admin.state() == "NORMAL", Admin.state())

for _ = 1, 3 do
    Admin.on_admin_scan("ADMIN0001")
end

Admin.on_admin_scan("ADMIN0002")
Harness.check("a different admin card starts the count again",
              Admin.state() == "ADMIN_SCAN_1", Admin.state())

------------------------------------------------------------
-- Timeouts (section 6)
------------------------------------------------------------

Harness.section("Timeouts")

Admin.reset()
Admin.on_admin_scan("ADMIN0001")
Admin.on_admin_scan("ADMIN0001")

Harness.check("a short gap does not reset it",
              Admin.process_timeout(Time.Now() + 5) == false)
Harness.check("still counting", Admin.state() == "ADMIN_SCAN_2", Admin.state())
Harness.check("past scan_timeout it resets",
              Admin.process_timeout(Time.Now() + 11) == true)
Harness.check("back to NORMAL", Admin.state() == "NORMAL", Admin.state())

for _ = 1, 5 do
    Admin.on_admin_scan("ADMIN0001")
end

Harness.check("the override is open", Admin.is_override_active() == true)
Harness.check("it survives a short pause",
              Admin.process_timeout(Time.Now() + 30) == false)
Harness.check("and lapses past override_timeout",
              Admin.process_timeout(Time.Now() + 61) == true)
Harness.check("leaving NORMAL", Admin.state() == "NORMAL", Admin.state())
Harness.check("process_timeout does nothing in NORMAL", Admin.process_timeout() == false)

------------------------------------------------------------
-- A user card inside the override (section 5)
------------------------------------------------------------

Harness.section("Admin override for a user card")

local expired = EmployeeDb.insert({
    username = "Lapsed contractor",
    card_code = "CON00001",
    role = "contractor",
    expire_at = "2000-01-01",
})

local valid = EmployeeDb.insert({
    username = "Current contractor",
    card_code = "CON00002",
    role = "contractor",
    expire_at = "2099-12-31",
})

local homeless = EmployeeDb.insert({
    username = "No locker",
    card_code = "CON00003",
    role = "contractor",
})

LockerDb.assign(1, expired)
LockerDb.set_status(1, LockerDb.STATUS.EXPIRED)
LockerDb.assign(2, valid)
LockerDb.assign(3, { card_code = "CON09999", role = "contractor" })
LockerDb.set_status(3, LockerDb.STATUS.EXPIRED)

Admin.reset()

for _ = 1, 5 do
    Admin.on_admin_scan("ADMIN0001")
end

unlocked = {}
result = Admin.process_user_card("CON00001")

Harness.check("an expired card's own locker opens", result.granted == true, result.reason)
Harness.check("reported as an override", result.event == "ADMIN_OVERRIDE_ACCESS", result.event)
Harness.check("the reason names why", result.reason == "EXPIRED_CONTRACTOR", result.reason)
Harness.check("locker 1 was the one opened",
              unlocked[1] and unlocked[1].locker_id == 1,
              unlocked[1] and unlocked[1].locker_id)
Harness.check("exactly one door", #unlocked == 1, #unlocked)
Harness.check("the mode stays open for the next person", Admin.is_override_active() == true)

result = Admin.process_user_card("CON00002")
Harness.check("a card that is not expired is served too", result.granted == true, result.reason)
Harness.check("as an ordinary admin request", result.reason == "ADMIN_REQUEST", result.reason)

result = Admin.process_user_card("NOSUCH01")
Harness.check("an unknown card is still unknown", result.granted == false)
Harness.check("as CARD_NOT_FOUND", result.event == "CARD_NOT_FOUND", result.event)
Harness.check("and the mode is still open", Admin.is_override_active() == true)

result = Admin.process_user_card("CON00003")
Harness.check("a card with no locker is refused", result.granted == false)
Harness.check("as NO_LOCKER_ASSIGNED", result.event == "NO_LOCKER_ASSIGNED", result.event)

Config.admin.override_expired = false
result = Admin.process_user_card("CON00001")
Harness.check("override_expired = false refuses an expired card",
              result.granted == false, result.reason)
Config.admin.override_expired = true

------------------------------------------------------------
-- Stepping through the expired lockers (section 4)
------------------------------------------------------------

Harness.section("Expired lockers, one door per scan")

-- Locker 6 has no unlock output, so it must never be offered.
LockerDb.assign(6, { card_code = "CON08888", role = "contractor" })
LockerDb.set_status(6, LockerDb.STATUS.EXPIRED)

Admin.reset()

for _ = 1, 5 do
    Admin.on_admin_scan("ADMIN0001")
end

unlocked = {}

local pending = Admin.pending_expired()

Harness.check("two expired lockers can be opened", #pending == 2, #pending)
Harness.check("the one with no output is not among them",
              pending[1].id ~= 6 and pending[2].id ~= 6)

result = Admin.on_admin_scan("ADMIN0001")

Harness.check("an admin scan opens one of them",
              result.event == "ADMIN_EXPIRED_LOCKER_OPEN", result.event)
Harness.check("the lowest-numbered one first",
              unlocked[1] and unlocked[1].locker_id == 1,
              unlocked[1] and unlocked[1].locker_id)
Harness.check("ONE door, not the whole cabinet", #unlocked == 1, #unlocked)

Admin.on_admin_scan("ADMIN0001")

Harness.check("the next scan steps to the next one",
              unlocked[2] and unlocked[2].locker_id == 3,
              unlocked[2] and unlocked[2].locker_id)
Harness.check("still one door at a time", #unlocked == 2, #unlocked)

result = Admin.on_admin_scan("ADMIN0001")

Harness.check("a further scan reports there is nothing left", result.granted == false, result.event)
Harness.check("and the mode is still open", Admin.is_override_active() == true)
Harness.check("no extra door was opened", #unlocked == 2, #unlocked)

-- With the stepping turned off, an admin scan only extends the mode.
Admin.reset()
Config.admin.scan_opens_expired = false

for _ = 1, 5 do
    Admin.on_admin_scan("ADMIN0001")
end

unlocked = {}
result = Admin.on_admin_scan("ADMIN0001")

Harness.check("scan_opens_expired = false opens nothing", #unlocked == 0, #unlocked)
Harness.check("it extends the mode instead",
              result.granted == true and result.event == "ADMIN_SCAN_SEQUENCE", result.event)

Config.admin.scan_opens_expired = true

------------------------------------------------------------
-- Opening one named locker
------------------------------------------------------------

Harness.section("Opening a named expired locker")

Admin.reset()

for _ = 1, 5 do
    Admin.on_admin_scan("ADMIN0001")
end

unlocked = {}

Harness.check("a locker that is not EXPIRED is refused",
              Admin.unlock_expired(2).granted == false)
Harness.check("a locker with no unlock output is refused",
              Admin.unlock_expired(6).granted == false)
Harness.check("a locker that does not exist is refused",
              Admin.unlock_expired(99).granted == false)
Harness.check("an expired locker opens", Admin.unlock_expired(3).granted == true)
Harness.check("with exactly one pulse", #unlocked == 1, #unlocked)

Harness.check("exit_override_mode closes it", Admin.exit_override_mode("test") == true)
Harness.check("leaving NORMAL", Admin.state() == "NORMAL", Admin.state())
Harness.check("closing it again reports nothing to do",
              Admin.exit_override_mode("test") == false)

------------------------------------------------------------
-- Through core/access.lua (section 8's priority order)
------------------------------------------------------------

Harness.section("Priority order at the reader")

Admin.reset()
unlocked = {}

for _ = 1, 4 do
    result = Access.on_card("ADMIN0001")
end

Harness.check("four admin swipes open nothing", #unlocked == 0, #unlocked)

result = Access.on_card("ADMIN0001")

Harness.check("the fifth opens admin mode", result.event == "ADMIN_MODE_ENTER", result.event)
Harness.check("and access reports it as an admin result", result.admin == true)

unlocked = {}
result = Access.on_card("CON00001")

Harness.check("an expired contractor is now served", result.granted == true, result.reason)
Harness.check("as an override", result.event == "ADMIN_OVERRIDE_ACCESS", result.event)
Harness.check("their own locker opened",
              unlocked[1] and unlocked[1].locker_id == 1,
              unlocked[1] and unlocked[1].locker_id)

Admin.exit_override_mode("end of test")

unlocked = {}
result = Access.on_card("CON00001")

Harness.check("with the mode closed the same card is refused again",
              result.granted == false, result.event)
Harness.check("as CARD_EXPIRED", result.event == "CARD_EXPIRED", result.event)
Harness.check("and nothing opened", #unlocked == 0, #unlocked)

Access.on_card("ADMIN0001")
Access.on_card("ADMIN0001")
Harness.check("a sequence can be started at the reader",
              Admin.state() == "ADMIN_SCAN_2", Admin.state())

Access.on_card("CON00002")
Harness.check("and an ordinary card breaks it", Admin.state() == "NORMAL", Admin.state())

restore()

Harness.finish()
