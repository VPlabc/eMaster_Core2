-- tests/test_locker.lua
-- Stage 5 and 6 (Plan sections 28, 29 and 30): assignment and card access.
--
-- Runs with no hardware. The PLC-facing calls are replaced with stubs, so the
-- rules being checked here -- locker type, gender priority, expiry, wrong
-- cabinet, no locker available -- are checked as rules and not as wiring. The
-- wiring has its own test (test_modbus_locker.lua).

local ok = pcall(require, "bootstrap")
if not ok then require("smartlocker.tests.bootstrap") end

local Access     = require("core.access")
local Assignment = require("core.assignment")
local Config     = require("config")
local Database   = require("database.database")
local Employee   = require("core.employee")
local EmployeeDb = require("database.employee_db")
local Harness    = require("tests.harness")
local Locker     = require("core.locker")
local LockerDb   = require("database.locker_db")
local Logger     = require("utils.logger")

Harness.start("Stage 5/6 -- assignment and access")

------------------------------------------------------------
-- A cabinet of each type, and no PLC
------------------------------------------------------------

local opened, openError = Harness.fresh_database()

if not Harness.check("test database", opened, openError) then
    Harness.finish()
    return
end

-- Two blocks: six employee lockers and six contractor lockers, the last two of
-- each with no unlock output (Plan section 6's lockers 7 and 8).
local layout = {}

local function block(blockId, name, lockerType, inputRegister, outputRegister)

    for number = 1, 6 do
        layout[#layout + 1] = {
            block_id = blockId,
            block_name = name,
            locker_number = number,
            locker_type = lockerType,
            input_register = inputRegister,
            input_bit = 7 + number,
            output_register = (number <= 4) and outputRegister or nil,
            output_bit = (number <= 4) and (7 + number) or nil,
        }
    end

end

block(1, "Employee", "employee", 30001, 40001)
block(2, "Contractor", "contractor", 30002, 40002)

LockerDb.ensure_layout(layout)

Harness.check("twelve lockers configured", #LockerDb.all() == 12)

-- The blocks the access check consults come from the configuration, so they are
-- pointed at this layout for the duration of the test.
Config.blocks = {
    { id = 1, name = "Employee", type = "employee", count = 6 },
    { id = 2, name = "Contractor", type = "contractor", count = 6 },
}

-- No PLC in this test: unlock succeeds and records that it was called, and the
-- door is reported closed so the state machine sits in UNLOCKING rather than
-- inventing transitions.
local unlocked = {}

Locker.unlock = function(lockerId, cardCode)
    unlocked[#unlocked + 1] = { locker_id = lockerId, card_code = cardCode }
    return true
end

Locker.is_open = function()
    return false
end

------------------------------------------------------------
-- Assignment: type
------------------------------------------------------------

Harness.section("Assignment -- locker type (Plan section 7)")

local alice = EmployeeDb.insert({ username = "NGUYEN VAN A", card_code = "ABCD0123",
                                  role = "employee", gender = "female", active = true })

local bob = EmployeeDb.insert({ username = "NGUYEN VAN B", card_code = "ABCD5678",
                                role = "contractor", gender = "male",
                                expire_at = "2099-12-31", active = true })

local aliceLocker = Assignment.assign(alice)
local bobLocker = Assignment.assign(bob)

Harness.check("the employee got an employee locker",
              aliceLocker ~= nil and aliceLocker.locker_type == "employee",
              aliceLocker and aliceLocker.locker_type)

Harness.check("the contractor got a contractor locker",
              bobLocker ~= nil and bobLocker.locker_type == "contractor",
              bobLocker and bobLocker.locker_type)

Harness.check("both sides of the relationship were written",
              EmployeeDb.find_by_card("ABCD0123").locker_id == aliceLocker.id
              and LockerDb.get(aliceLocker.id).card_code == "ABCD0123")

Harness.check("assigning again returns the same locker",
              Assignment.assign(alice).id == aliceLocker.id)

Harness.check("the lowest free number is used", aliceLocker.locker_number == 1,
              "got " .. tostring(aliceLocker.locker_number))

------------------------------------------------------------
-- Assignment: gender priority (Plan section 29)
------------------------------------------------------------

Harness.section("Assignment -- gender")

-- Default configuration: both genders take the lowest free locker.
local carol = EmployeeDb.insert({ username = "C", card_code = "C0000001",
                                  role = "employee", gender = "female", active = true })

local dave = EmployeeDb.insert({ username = "D", card_code = "D0000001",
                                 role = "employee", gender = "male", active = true })

local carolLocker = Assignment.assign(carol)
local daveLocker = Assignment.assign(dave)

Harness.check("female took the next lowest", carolLocker.locker_number == 2)
Harness.check("male took the one after", daveLocker.locker_number == 3)

-- With the optional reserved range switched on, a woman is offered it first.
Assignment.release(carol.card_code, "test")
Assignment.release(dave.card_code, "test")
Assignment.release(alice.card_code, "test")

Config.locker.female_priority = { enabled = true, start = 3, ["end"] = 4 }

local eve = EmployeeDb.insert({ username = "E", card_code = "E0000001",
                                role = "employee", gender = "female", active = true })

local eveLocker = Assignment.assign(eve)

Harness.check("female priority range is used first", eveLocker.locker_number == 3,
              "got " .. tostring(eveLocker.locker_number))

local frank = EmployeeDb.insert({ username = "F", card_code = "F0000001",
                                  role = "employee", gender = "male", active = true })

Harness.check("a man still takes the lowest free locker",
              Assignment.assign(frank).locker_number == 1)

Config.locker.female_priority = { enabled = false }

------------------------------------------------------------
-- Assignment: exhaustion
------------------------------------------------------------

Harness.section("Assignment -- no locker available (Plan section 7)")

-- Four employee lockers have outputs; two are taken above.
local extras = {}

for index = 1, 4 do

    local row = EmployeeDb.insert({ username = "X" .. index, card_code = "X000000" .. index,
                                    role = "employee", active = true })

    extras[#extras + 1] = { row = row, locker = Assignment.assign(row) }

end

local assignedCount = 0

for _, entry in ipairs(extras) do
    if entry.locker ~= nil then
        assignedCount = assignedCount + 1
    end
end

Harness.check("only the lockers with outputs were handed out", assignedCount == 2,
              assignedCount .. " of 4 extra employees got a locker")

local unlucky = EmployeeDb.insert({ username = "Z", card_code = "Z0000001", role = "employee", active = true })
local none, noneReason = Assignment.assign(unlucky)

Harness.check("assignment fails cleanly when nothing is free", none == nil, noneReason)
Harness.check("the employee is left with no locker", EmployeeDb.find_by_card("Z0000001").locker_id == nil)

------------------------------------------------------------
-- Access (Plan sections 8, 9, 10)
------------------------------------------------------------

Harness.section("Access")

Config.locker.assign_on_scan = false   -- so the denials below stay denials

local function scan(cardCode, blockId)
    return Access.on_card(cardCode, { block_id = blockId or 1, source = "test" })
end

-- Unknown card.
local unknown = scan("NOSUCHCARD")

Harness.check("unknown card denied", unknown.granted == false)
Harness.check("  event is CARD_NOT_FOUND", unknown.event == Logger.EVENTS.CARD_NOT_FOUND, unknown.event)

-- Inactive card.
EmployeeDb.set_active(bob.id, false)

local inactive = scan("ABCD5678", 2)

Harness.check("inactive card denied", inactive.granted == false)
Harness.check("  event is CARD_INACTIVE", inactive.event == Logger.EVENTS.CARD_INACTIVE, inactive.event)

EmployeeDb.set_active(bob.id, true)

-- Expired contractor.
EmployeeDb.update(bob.id, { expire_at = os.date("%Y-%m-%d", os.time() - 86400) })

local expired = scan("ABCD5678", 2)

Harness.check("expired contractor denied", expired.granted == false)
Harness.check("  event is CARD_EXPIRED", expired.event == Logger.EVENTS.CARD_EXPIRED, expired.event)
Harness.check("  the locker is marked EXPIRED",
              LockerDb.get(bobLocker.id).status == LockerDb.STATUS.EXPIRED,
              LockerDb.get(bobLocker.id).status)

EmployeeDb.update(bob.id, { expire_at = "2099-12-31" })
LockerDb.set_status(bobLocker.id, LockerDb.STATUS.ASSIGNED)

-- Wrong cabinet: the contractor presents their card at the employee block.
local wrongBlock = scan("ABCD5678", 1)

Harness.check("wrong cabinet denied", wrongBlock.granted == false)
Harness.check("  event is WRONG_LOCKER_TYPE", wrongBlock.event == Logger.EVENTS.WRONG_LOCKER_TYPE,
              wrongBlock.event)

-- No locker assigned.
local homeless = scan("Z0000001", 1)

Harness.check("a card with no locker is denied", homeless.granted == false)
Harness.check("  event is NO_LOCKER_ASSIGNED", homeless.event == Logger.EVENTS.NO_LOCKER_ASSIGNED,
              homeless.event)

-- The happy path.
unlocked = {}

local granted = scan("ABCD5678", 2)

Harness.check("a valid contractor at their own cabinet is granted", granted.granted == true, granted.event)
Harness.check("  the unlock reached the right locker",
              #unlocked == 1 and unlocked[1].locker_id == bobLocker.id)

-- A card presented twice while the door is still in its cycle.
Locker.unlock = function(lockerId, cardCode)
    unlocked[#unlocked + 1] = { locker_id = lockerId, card_code = cardCode }
    return true
end

------------------------------------------------------------
-- Assignment consistency
------------------------------------------------------------

Harness.section("Reconciliation")

-- Break the relationship in both directions and let reconcile() repair it.
LockerDb.update(aliceLocker.id, { card_code = "GHOST001", status = LockerDb.STATUS.ASSIGNED })
EmployeeDb.update(alice.id, { locker_id = 999 })

local report = Assignment.reconcile()

Harness.check("a locker holding an unknown card is released", report.orphan_lockers >= 1)
Harness.check("an employee pointing at nothing is cleared", report.orphan_employees >= 1)
Harness.check("  the locker is empty again", LockerDb.get(aliceLocker.id).status == LockerDb.STATUS.EMPTY)
Harness.check("  the employee link is gone", EmployeeDb.find_by_card("ABCD0123").locker_id == nil)

------------------------------------------------------------
-- Release
------------------------------------------------------------

Harness.section("Release")

local before = #LockerDb.available("contractor")

Assignment.release("ABCD5678", "test")

Harness.check("release frees the locker", #LockerDb.available("contractor") == before + 1)
Harness.check("  the employee no longer points at it",
              EmployeeDb.find_by_card("ABCD5678").locker_id == nil)
Harness.check("releasing an unassigned card is harmless", Assignment.release("ABCD5678", "test") == true)

Harness.info("")
Harness.info("Access counters: " .. require("utils.json").encode(Access.stats()))

Harness.finish()
