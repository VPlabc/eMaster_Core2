-- core/employee.lua
-- Employee and contractor records (Plan sections 2 and 27). This module owns
-- what a record MEANS: which role it is, whether it is still valid today, and
-- how a server record is reconciled with the local one.
--
-- Rules from section 2, in one place:
--   * employees normally carry no expiry and are not expiration-checked
--   * contractors must carry one, and an expired contractor is refused
--   * an inactive card is refused
--   * an unknown card is refused
--
-- Storage lives in database/employee_db.lua; nothing here writes a file.

local Config     = require("config")
local Database   = require("database.database")
local EmployeeDb = require("database.employee_db")
-- Json is still needed for the INPUT side: a record decoded from the server or
-- the broker carries Json.null where the payload said null, and that has to be
-- told apart from a field the payload simply omitted.
local Json       = require("utils.json")
local Logger     = require("utils.logger")
local Time       = require("utils.time")

local Employee = {}

Employee.ROLE = {
    EMPLOYEE   = "employee",
    CONTRACTOR = "contractor",
}

Employee.GENDER = {
    MALE   = "male",
    FEMALE = "female",
}

------------------------------------------------------------
-- Normalisation
------------------------------------------------------------

-- Role text as it actually arrives. Matched as a lowercased substring so
-- "Contractor", "NHA THAU" and "external contractor" all land on the same role
-- -- the same approach the main gateway's config.lua takes for its two card
-- machines, and for the same reason: the server's wording is not ours to fix.
local ROLE_PATTERNS = {
    { pattern = "contractor",  role = Employee.ROLE.CONTRACTOR },
    { pattern = "nha thau",    role = Employee.ROLE.CONTRACTOR },
    { pattern = "nhà thầu",    role = Employee.ROLE.CONTRACTOR },
    { pattern = "vendor",      role = Employee.ROLE.CONTRACTOR },
    { pattern = "subcontract", role = Employee.ROLE.CONTRACTOR },
    { pattern = "guest",       role = Employee.ROLE.CONTRACTOR },

    { pattern = "employee",    role = Employee.ROLE.EMPLOYEE },
    { pattern = "staff",       role = Employee.ROLE.EMPLOYEE },
    { pattern = "nhan vien",   role = Employee.ROLE.EMPLOYEE },
    { pattern = "nhân viên",   role = Employee.ROLE.EMPLOYEE },
}

local GENDER_PATTERNS = {
    { pattern = "female", gender = Employee.GENDER.FEMALE },
    { pattern = "woman",  gender = Employee.GENDER.FEMALE },
    { pattern = "^f$",    gender = Employee.GENDER.FEMALE },
    { pattern = "nu",     gender = Employee.GENDER.FEMALE },
    { pattern = "nữ",     gender = Employee.GENDER.FEMALE },

    { pattern = "male",   gender = Employee.GENDER.MALE },
    { pattern = "man",    gender = Employee.GENDER.MALE },
    { pattern = "^m$",    gender = Employee.GENDER.MALE },
    { pattern = "nam",    gender = Employee.GENDER.MALE },
}

function Employee.normalize_role(value)

    if value == nil then
        return nil
    end

    local text = tostring(value):lower()

    -- Female-first ordering matters for the gender table below, and
    -- contractor-first here: "employee of a contractor" must not read as an
    -- employee. Longest-meaning wins by being listed first.
    for _, entry in ipairs(ROLE_PATTERNS) do
        if text:find(entry.pattern, 1, true) then
            return entry.role
        end
    end

    return nil

end

function Employee.normalize_gender(value)

    if value == nil then
        return nil
    end

    local text = tostring(value):lower():gsub("^%s+", ""):gsub("%s+$", "")

    for _, entry in ipairs(GENDER_PATTERNS) do

        -- "nam" (male) is a substring of nothing dangerous, but "nu" is a
        -- substring of plenty, so anchored patterns are used for the short
        -- ones and find() is given the pattern flag here.
        if entry.pattern:sub(1, 1) == "^" then
            if text:find(entry.pattern) then
                return entry.gender
            end
        elseif text:find(entry.pattern, 1, true) then
            return entry.gender
        end

    end

    return nil

end

------------------------------------------------------------
-- Validation
------------------------------------------------------------

-- Employee.validate(record) -> normalized | nil, reason
--
-- Strict about what makes a record usable (a card code and a resolvable role),
-- forgiving about the rest: a missing gender only costs the female-priority
-- rule, and refusing the record over it would leave a real person with no
-- locker at all.
function Employee.validate(record)

    if type(record) ~= "table" then
        return nil, "record is not a table"
    end

    local cardCode = EmployeeDb.normalize_code(record.card_code)

    if cardCode == nil then
        return nil, "record has no card_code"
    end

    local role = Employee.normalize_role(record.role)

    if role == nil then

        -- An unrecognised role is defaulted rather than rejected, but loudly:
        -- the wrong default hands someone a locker in the wrong cabinet, and
        -- that has to be visible on the Logs page.
        role = Employee.ROLE.EMPLOYEE

        if record.role ~= nil and record.role ~= "" then
            Logger.warning("card " .. cardCode .. ": unrecognised role '" .. tostring(record.role) ..
                           "', treating as " .. role)
        end

    end

    local expireAt = record.expire_at

    if expireAt == "" or expireAt == Json.null then
        expireAt = nil
    end

    if expireAt ~= nil and Time.Parse(expireAt) == nil then
        return nil, "card " .. cardCode .. ": expire_at '" .. tostring(expireAt) .. "' is not a date"
    end

    -- Section 2: "Contractors must have an expiration date." A contractor
    -- without one is accepted -- refusing it would deny a real person their
    -- locker over the server's omission -- but it is recorded, because a
    -- contractor card that never expires is a finding, not a detail.
    if role == Employee.ROLE.CONTRACTOR and expireAt == nil then
        Logger.warning("contractor card " .. cardCode .. " has no expire_at; it will never expire")
    end

    -- CardScanPlan section 1's contractor_start_date. Unlike expire_at an
    -- unreadable value is DROPPED rather than rejected: the card is still
    -- valid, it just loses the "not before" check, whereas a record refused
    -- here would leave a working contractor with no record at all.
    local startAt = record.start_at

    if startAt == "" or startAt == Json.null then
        startAt = nil
    end

    if startAt ~= nil and Time.Parse(startAt) == nil then
        Logger.warning("card " .. cardCode .. ": start_at '" .. tostring(startAt) ..
                       "' is not a date; ignoring it")
        startAt = nil
    end

    return {
        username = record.username and tostring(record.username) or "",
        card_code = cardCode,
        role = role,
        gender = Employee.normalize_gender(record.gender),
        expire_at = expireAt,
        start_at = startAt,
        active = record.active ~= false,
        locker_id = record.locker_id,
    }

end

------------------------------------------------------------
-- Queries
------------------------------------------------------------

function Employee.init()
    -- The table needs no preparation -- database.open() created it. Kept so
    -- main.lua's start-up sequence reads as the plan writes it, and so a future
    -- index or cache has an obvious home.
    return true
end

function Employee.find_by_card(cardCode)
    return EmployeeDb.find_by_card(cardCode)
end

function Employee.all()
    return EmployeeDb.all()
end

function Employee.active()
    return EmployeeDb.active()
end

function Employee.is_active(employee)
    return employee ~= nil and employee.active == true
end

-- Employees have no expiry and are not checked (section 10). Contractors are.
function Employee.is_expired(employee, now)

    if employee == nil then
        return false
    end

    if employee.expire_at == nil or employee.expire_at == "" then
        return false
    end

    local expired = Time.IsExpired(employee.expire_at, now, Config.sync.expire_at_end_of_day)

    return expired

end

-- One word for the frontend and the logs: ACTIVE, INACTIVE or EXPIRED.
function Employee.status(employee)

    if employee == nil then
        return "UNKNOWN"
    end

    if not Employee.is_active(employee) then
        return "INACTIVE"
    end

    if Employee.is_expired(employee) then
        return "EXPIRED"
    end

    return "ACTIVE"

end

------------------------------------------------------------
-- Mutations
------------------------------------------------------------

function Employee.add(record)

    local validated, err = Employee.validate(record)

    if validated == nil then
        return nil, err
    end

    if EmployeeDb.find_by_card(validated.card_code) ~= nil then
        return nil, "card " .. validated.card_code .. " already exists"
    end

    return EmployeeDb.insert(validated)

end

function Employee.update(record)

    local validated, err = Employee.validate(record)

    if validated == nil then
        return nil, err
    end

    local existing = EmployeeDb.find_by_card(validated.card_code)

    if existing == nil then
        return nil, "card " .. validated.card_code .. " is not in the database"
    end

    return EmployeeDb.update(existing.id, {
        username = validated.username,
        role = validated.role,
        gender = validated.gender,
        expire_at = validated.expire_at or Database.NULL,
        start_at = validated.start_at or Database.NULL,
        active = validated.active,
    })

end

-- Employee.upsert(record) -> row, action, error
--
-- `action` is "added", "updated" or "unchanged" -- the synchronisation counts
-- them, and "unchanged" is what keeps a daily sync of 400 cards from writing
-- 400 rows and 400 log entries.
function Employee.upsert(record)

    local validated, err = Employee.validate(record)

    if validated == nil then
        return nil, nil, err
    end

    local existing = EmployeeDb.find_by_card(validated.card_code)

    if existing == nil then
        return EmployeeDb.insert(validated), "added"
    end

    local changes = {}

    if existing.username ~= validated.username and validated.username ~= "" then
        changes.username = validated.username
    end

    if existing.role ~= validated.role then
        changes.role = validated.role
    end

    if validated.gender ~= nil and existing.gender ~= validated.gender then
        changes.gender = validated.gender
    end

    if existing.expire_at ~= validated.expire_at then
        changes.expire_at = validated.expire_at or Database.NULL
    end

    -- A record that simply does not mention start_at must not CLEAR one the
    -- server sent yesterday: only an explicit change is written. That is the
    -- difference from expire_at above, where a card losing its expiry is a real
    -- and meaningful update.
    if validated.start_at ~= nil and existing.start_at ~= validated.start_at then
        changes.start_at = validated.start_at
    end

    if existing.active ~= validated.active then
        changes.active = validated.active
    end

    if next(changes) == nil then
        return existing, "unchanged"
    end

    return EmployeeDb.update(existing.id, changes), "updated"

end

function Employee.deactivate(cardCode, reason)

    local existing = EmployeeDb.find_by_card(cardCode)

    if existing == nil then
        return nil, "unknown card " .. tostring(cardCode)
    end

    if existing.active == false then
        return existing, "unchanged"
    end

    Logger.info("deactivating card " .. tostring(cardCode) .. (reason and (" -- " .. reason) or ""))

    return EmployeeDb.set_active(existing.id, false), "updated"

end

function Employee.remove(cardCode)
    return EmployeeDb.remove(cardCode)
end

------------------------------------------------------------
-- Synchronisation
------------------------------------------------------------

-- Employee.sync(list) -> stats, changes
--
-- Reconciles the local table with a full list from the server (Plan section
-- 3.1). It only touches the employees table; the locker side of the change --
-- assigning the new cards, releasing the removed ones -- is core/assignment's
-- job, and the `changes` return value is what it works from. Keeping the two
-- apart is what allows the RabbitMQ path (section 3.2) to reuse the same
-- assignment code for a single record.
--
-- `on_removed` decides the fate of a card the server no longer lists:
--   "release" -- deactivate it and free its locker (the reconciling behaviour)
--   "keep"    -- deactivate only
function Employee.sync(list, options)

    options = options or {}

    local stats = { added = 0, updated = 0, unchanged = 0, deactivated = 0, invalid = 0 }
    local changes = { added = {}, updated = {}, deactivated = {} }

    if type(list) ~= "table" then
        return stats, changes, "sync list is not a table"
    end

    local seen = {}

    for _, record in ipairs(list) do

        local row, action, err = Employee.upsert(record)

        if row == nil then

            stats.invalid = stats.invalid + 1
            Logger.warning("sync: skipping a record -- " .. tostring(err))

        else

            seen[#seen + 1] = row.card_code

            if action == "added" then
                stats.added = stats.added + 1
                changes.added[#changes.added + 1] = row
            elseif action == "updated" then
                stats.updated = stats.updated + 1
                changes.updated[#changes.updated + 1] = row
            else
                stats.unchanged = stats.unchanged + 1
            end

        end

    end

    -- Cards the server did not mention. A partial or failed download must never
    -- reach this point -- an empty list would deactivate the entire building --
    -- so the caller passes options.full_list = true only for a response it
    -- trusts, and core/sync.lua additionally refuses an empty one.
    if options.full_list then

        for _, row in ipairs(EmployeeDb.missing_from(seen)) do

            if row.active ~= false then

                EmployeeDb.set_active(row.id, false)

                stats.deactivated = stats.deactivated + 1
                changes.deactivated[#changes.deactivated + 1] = row

            end

        end

    end

    return stats, changes

end

-- Re-checks every contractor's expiry. Called on the daily sync and at
-- start-up, so a card that lapsed overnight is EXPIRED on the status page
-- before anyone swipes it.
function Employee.refresh_expiry()

    local expired = {}
    local now = Time.Now()

    for _, row in ipairs(EmployeeDb.all()) do

        if Employee.is_expired(row, now) then
            expired[#expired + 1] = row
        end

    end

    return expired

end

return Employee
