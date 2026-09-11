-- database/employee_db.lua
-- Row access for the `employees` table (Plan section 15). SQL lives here and in
-- locker_db.lua; nothing above these two files writes a statement.
--
-- Columns:
--   id, username, card_code, role, gender, expire_at, active, locker_id,
--   start_at, usage_date, usage_started_at, usage_last_open_at,
--   usage_completed_at, usage_status, created_at, updated_at

local Database = require("database.database")

local EmployeeDb = {}

local TABLE = "employees"

-- Every column the rest of the application reads, in one place, so a query
-- cannot quietly return a row that is missing a field a caller expects.
local FIELDS = "id, username, card_code, role, gender, expire_at, active, locker_id, " ..
               "start_at, usage_date, usage_started_at, usage_last_open_at, " ..
               "usage_completed_at, usage_status, created_at, updated_at"

-- Card codes are compared in exactly one form everywhere: trimmed and upper
-- case. A reader that reports "abcd0123" and a server that sends "ABCD0123"
-- describe the same card, and a lookup that misses would hand the person an
-- "unknown card" beep while their record sits in the table.
function EmployeeDb.normalize_code(cardCode)

    if cardCode == nil then
        return nil
    end

    local text = tostring(cardCode):gsub("%s+", ""):upper()

    if text == "" then
        return nil
    end

    return text

end

-- SQLite has no boolean column, so `active` comes back as 0/1. Converting it
-- here means every caller can keep writing `row.active == false`.
local function map(row)

    if row == nil then
        return nil
    end

    row.active = Database.to_boolean(row.active)

    return row

end

local function mapAll(rows)

    for _, row in ipairs(rows or {}) do
        map(row)
    end

    return rows or {}

end

function EmployeeDb.all()
    return mapAll(Database.query("SELECT " .. FIELDS .. " FROM " .. TABLE .. " ORDER BY id"))
end

function EmployeeDb.count()
    return Database.count(TABLE)
end

function EmployeeDb.find_by_card(cardCode)

    local code = EmployeeDb.normalize_code(cardCode)

    if code == nil then
        return nil
    end

    return map(Database.query_one(
        "SELECT " .. FIELDS .. " FROM " .. TABLE .. " WHERE card_code = ?", { code }))

end

function EmployeeDb.find_by_id(id)
    return map(Database.query_one("SELECT " .. FIELDS .. " FROM " .. TABLE .. " WHERE id = ?", { id }))
end

function EmployeeDb.find_by_locker(lockerId)
    return map(Database.query_one(
        "SELECT " .. FIELDS .. " FROM " .. TABLE .. " WHERE locker_id = ?", { lockerId }))
end

function EmployeeDb.active()
    return mapAll(Database.query(
        "SELECT " .. FIELDS .. " FROM " .. TABLE .. " WHERE active = 1 ORDER BY username"))
end

function EmployeeDb.by_role(role)
    return mapAll(Database.query(
        "SELECT " .. FIELDS .. " FROM " .. TABLE .. " WHERE role = ? ORDER BY id", { role }))
end

-- Cards whose usage record belongs to a day that is over (CardScanPlan section
-- 1: a morning open with no afternoon open is still a used day, and the day
-- itself ends at midnight). Answered by SQLite rather than by walking every
-- card in Lua, because this runs on a timer and the table is the whole site.
--
-- `usage_date` is stored as YYYY-MM-DD, so the string comparison is the date
-- comparison.
function EmployeeDb.stale_usage(today, statuses)

    if today == nil then
        return {}
    end

    statuses = statuses or { "USING" }

    local placeholders = {}
    local params = { today }

    for _, status in ipairs(statuses) do
        placeholders[#placeholders + 1] = "?"
        params[#params + 1] = status
    end

    if #placeholders == 0 then
        return {}
    end

    return mapAll(Database.query(
        "SELECT " .. FIELDS .. " FROM " .. TABLE ..
        " WHERE usage_date IS NOT NULL AND usage_date < ?" ..
        " AND usage_status IN (" .. table.concat(placeholders, ", ") .. ") ORDER BY id", params))

end

-- Returns the stored row (re-read, so ids and timestamps are the database's own
-- rather than what the caller guessed), or nil plus the reason.
function EmployeeDb.insert(record)

    local id, err = Database.insert(TABLE, {
        username = record.username or "",
        card_code = EmployeeDb.normalize_code(record.card_code),
        role = record.role or "employee",
        gender = record.gender,
        expire_at = record.expire_at,
        start_at = record.start_at,
        active = (record.active ~= false) and 1 or 0,
        locker_id = record.locker_id,
        -- A card that has never been presented has no usage day. The column
        -- default says NOT_STARTED for exactly this reason; it is repeated here
        -- so a row inserted through this path reads the same whether SQLite or
        -- Lua supplied it.
        usage_status = record.usage_status or "NOT_STARTED",
    })

    if id == nil then
        return nil, err
    end

    return EmployeeDb.find_by_id(id)

end

-- `fields` follows the Database.update convention: Database.NULL clears a
-- column, which is how a contractor promoted to employee loses their expiry.
function EmployeeDb.update(id, fields)

    local patch = {}

    for key, value in pairs(fields) do
        patch[key] = value
    end

    if patch.card_code ~= nil and patch.card_code ~= Database.NULL then
        patch.card_code = EmployeeDb.normalize_code(patch.card_code)
    end

    if patch.active ~= nil and patch.active ~= Database.NULL then
        patch.active = patch.active and 1 or 0
    end

    local changes = Database.update(TABLE, id, patch)

    if changes == nil then
        return nil, "update failed"
    end

    return EmployeeDb.find_by_id(id)

end

function EmployeeDb.set_active(id, active)
    return EmployeeDb.update(id, { active = active == true })
end

-- nil detaches the employee from their locker; locker_db keeps the other half
-- of the relationship and both are written inside one transaction by
-- core/assignment.lua.
function EmployeeDb.set_locker(id, lockerId)
    return EmployeeDb.update(id, { locker_id = lockerId or Database.NULL })
end

function EmployeeDb.remove(cardCode)

    local row = EmployeeDb.find_by_card(cardCode)

    if row == nil then
        return false
    end

    local changes = Database.delete(TABLE, row.id)

    return (changes or 0) > 0

end

-- Every card the server did not mention in this synchronisation run.
--
-- Done as one statement with a temporary table rather than "SELECT everything
-- and filter in Lua": a full sync of 400 cards would otherwise pull the whole
-- table into the VM to answer a question SQLite can answer itself.
function EmployeeDb.missing_from(codes)

    if type(codes) ~= "table" or #codes == 0 then
        return EmployeeDb.all()
    end

    local placeholders, params = {}, {}

    for _, code in ipairs(codes) do
        local normalized = EmployeeDb.normalize_code(code)
        if normalized then
            placeholders[#placeholders + 1] = "?"
            params[#params + 1] = normalized
        end
    end

    if #placeholders == 0 then
        return EmployeeDb.all()
    end

    return mapAll(Database.query(
        "SELECT " .. FIELDS .. " FROM " .. TABLE ..
        " WHERE card_code NOT IN (" .. table.concat(placeholders, ", ") .. ") ORDER BY id", params))

end

return EmployeeDb
