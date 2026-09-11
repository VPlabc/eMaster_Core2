-- database/locker_db.lua
-- Row access for the `lockers` table (Plan section 15). Storage only: the state
-- machine and the door timers live in core/locker.lua.
--
-- Columns:
--   id, block_id, block_name, locker_number, locker_type, input_register,
--   input_bit, output_register, output_bit, status, card_code, assigned_at,
--   expire_at, last_open_at, last_close_at, last_error, door_open,
--   runtime_state, created_at, updated_at
--
-- `status` holds only the four PERMANENT states of Plan section 12 -- EMPTY,
-- ASSIGNED, EXPIRED, ERROR. The temporary ones (UNLOCKING, OPEN, WAIT_CLOSE)
-- describe a door in the middle of one swipe; they are mirrored into
-- `runtime_state` for the dashboard but are never read back as truth. After a
-- restart the gateway asks the PLC what the doors are doing (Plan section 37).

local Database = require("database.database")

local LockerDb = {}

local TABLE = "lockers"

local FIELDS = "id, block_id, block_name, locker_number, locker_type, input_register, input_bit, " ..
                "output_register, output_bit, status, card_code, assigned_at, expire_at, " ..
                "last_open_at, last_close_at, last_error, door_open, runtime_state, updated_at"

LockerDb.STATUS = {
    EMPTY    = "EMPTY",
    ASSIGNED = "ASSIGNED",
    EXPIRED  = "EXPIRED",
    ERROR    = "ERROR",
}

-- Columns that come from the configuration rather than from operation. These
-- are refreshed on every start, so re-wiring a cabinet is a configuration edit
-- and not a database migration.
local MAPPING_COLUMNS = {
    "block_id", "block_name", "locker_number", "locker_type",
    "input_register", "input_bit", "output_register", "output_bit",
}

local function map(row)

    if row == nil then
        return nil
    end

    row.door_open = Database.to_boolean(row.door_open)

    return row

end

local function mapAll(rows)

    for _, row in ipairs(rows or {}) do
        map(row)
    end

    return rows or {}

end

------------------------------------------------------------
-- Layout
------------------------------------------------------------

-- Brings the table in line with the configured layout, preserving every
-- assignment that still has a locker to live on. Returns added, updated,
-- removed counts.
--
-- Matching is by (block_id, locker_number), not by row id: ids shift when a
-- block grows, and an assignment must follow the physical door rather than the
-- row that happened to describe it.
function LockerDb.ensure_layout(definitions)

    local added, updated = 0, 0
    local seen = {}

    -- One transaction for the whole layout: a cabinet is either described or it
    -- is not, and a half-applied layout would leave doors the access path can
    -- find and the dashboard cannot draw.
    local ok, err = Database.transaction_do(function()

        for _, definition in ipairs(definitions) do

            seen[tostring(definition.block_id) .. ":" .. tostring(definition.locker_number)] = true

            local row = Database.query_one(
                "SELECT " .. FIELDS .. " FROM " .. TABLE .. " WHERE block_id = ? AND locker_number = ?",
                { definition.block_id, definition.locker_number })

            if row == nil then

                Database.insert(TABLE, {
                    block_id = definition.block_id,
                    block_name = definition.block_name,
                    locker_number = definition.locker_number,
                    locker_type = definition.locker_type,
                    input_register = definition.input_register,
                    input_bit = definition.input_bit,
                    output_register = definition.output_register,
                    output_bit = definition.output_bit,
                    status = LockerDb.STATUS.EMPTY,
                    door_open = 0,
                    runtime_state = "IDLE",
                })

                added = added + 1

            else

                local changes = {}

                for _, column in ipairs(MAPPING_COLUMNS) do

                    local wanted = definition[column]

                    if row[column] ~= wanted then
                        changes[column] = (wanted == nil) and Database.NULL or wanted
                    end

                end

                if next(changes) ~= nil then
                    Database.update(TABLE, row.id, changes)
                    updated = updated + 1
                end

            end

        end

        return true

    end)

    if not ok then
        return 0, 0, 0, "layout not applied: " .. tostring(err)
    end

    -- Doors that no longer exist in the configuration. Their rows go, but the
    -- employees who were sitting in them are left pointing at nothing on
    -- purpose: core/assignment.lua's reconciliation notices and gives them a
    -- new locker, which is a decision that belongs there and not here.
    local removed = 0

    for _, row in ipairs(LockerDb.all()) do
        if not seen[tostring(row.block_id) .. ":" .. tostring(row.locker_number)] then
            Database.delete(TABLE, row.id)
            removed = removed + 1
        end
    end

    return added, updated, removed

end

------------------------------------------------------------
-- Queries
------------------------------------------------------------

function LockerDb.all()
    return mapAll(Database.query(
        "SELECT " .. FIELDS .. " FROM " .. TABLE .. " ORDER BY block_id, locker_number"))
end

function LockerDb.get(lockerId)
    return map(Database.query_one("SELECT " .. FIELDS .. " FROM " .. TABLE .. " WHERE id = ?", { lockerId }))
end

function LockerDb.get_by_number(blockId, number)
    return map(Database.query_one(
        "SELECT " .. FIELDS .. " FROM " .. TABLE .. " WHERE block_id = ? AND locker_number = ?",
        { blockId, number }))
end

function LockerDb.get_by_card(cardCode)

    if cardCode == nil then
        return nil
    end

    return map(Database.query_one(
        "SELECT " .. FIELDS .. " FROM " .. TABLE .. " WHERE card_code = ?", { tostring(cardCode):upper() }))

end

function LockerDb.by_status(status)
    return mapAll(Database.query(
        "SELECT " .. FIELDS .. " FROM " .. TABLE .. " WHERE status = ? ORDER BY block_id, locker_number",
        { status }))
end

function LockerDb.by_type(lockerType)
    return mapAll(Database.query(
        "SELECT " .. FIELDS .. " FROM " .. TABLE .. " WHERE locker_type = ? ORDER BY block_id, locker_number",
        { lockerType }))
end

-- Free lockers of one type, lowest number first. "Free" means EMPTY, with no
-- card on it, and with an unlock output actually wired -- a door the PLC cannot
-- open is not available to be handed out, however empty it is (Plan section 6:
-- lockers 7 and 8 have no output).
function LockerDb.available(lockerType, blockId)

    local sql = "SELECT " .. FIELDS .. " FROM " .. TABLE ..
                " WHERE status = ? AND card_code IS NULL" ..
                "   AND output_register IS NOT NULL AND output_bit IS NOT NULL"

    local params = { LockerDb.STATUS.EMPTY }

    if lockerType ~= nil then
        sql = sql .. " AND locker_type = ?"
        params[#params + 1] = lockerType
    end

    if blockId ~= nil then
        sql = sql .. " AND block_id = ?"
        params[#params + 1] = blockId
    end

    sql = sql .. " ORDER BY block_id, locker_number"

    return mapAll(Database.query(sql, params))

end

function LockerDb.can_unlock(row)
    return row ~= nil and row.output_register ~= nil and row.output_bit ~= nil
end

------------------------------------------------------------
-- Mutations
------------------------------------------------------------

function LockerDb.update(lockerId, fields)

    local changes = Database.update(TABLE, lockerId, fields)

    if changes == nil then
        return nil, "update failed"
    end

    return LockerDb.get(lockerId)

end

function LockerDb.set_status(lockerId, status, reason)

    local fields = { status = status }

    if status == LockerDb.STATUS.ERROR then
        fields.last_error = reason
    elseif reason == nil then
        fields.last_error = Database.NULL
    end

    return LockerDb.update(lockerId, fields)

end

function LockerDb.assign(lockerId, employee, timestamp)

    return LockerDb.update(lockerId, {
        status = LockerDb.STATUS.ASSIGNED,
        card_code = employee.card_code,
        assigned_at = timestamp,
        expire_at = employee.expire_at or Database.NULL,
        last_error = Database.NULL,
    })

end

function LockerDb.release(lockerId)

    return LockerDb.update(lockerId, {
        status = LockerDb.STATUS.EMPTY,
        card_code = Database.NULL,
        assigned_at = Database.NULL,
        expire_at = Database.NULL,
        last_error = Database.NULL,
    })

end

function LockerDb.mark_open(lockerId, timestamp)
    return LockerDb.update(lockerId, { last_open_at = timestamp })
end

function LockerDb.mark_closed(lockerId, timestamp)
    return LockerDb.update(lockerId, { last_close_at = timestamp })
end

function LockerDb.set_error(lockerId, reason)
    return LockerDb.set_status(lockerId, LockerDb.STATUS.ERROR, reason)
end

-- The two runtime mirrors the dashboard reads. Written ONLY on a change (see
-- core/locker.lua): a poll that wrote the door state every 100 ms would turn a
-- quiet cabinet into a few hundred thousand pointless UPDATEs a day, and every
-- one of them would wake the UI's change detection.
function LockerDb.set_door(lockerId, open)
    return LockerDb.update(lockerId, { door_open = open and 1 or 0 })
end

function LockerDb.set_runtime_state(lockerId, runtimeState)
    return LockerDb.update(lockerId, { runtime_state = runtimeState or "IDLE" })
end

------------------------------------------------------------
-- Audit trail
------------------------------------------------------------

-- One row per access/door event -- Plan section 14's locker_logs, and what the
-- floor-plan UI shows as "Hoạt động gần đây". utils/logger.lua is the only
-- caller; everything else logs through it.
function LockerDb.log(entry)

    return Database.insert("locker_logs", {
        locker_id = entry.locker_id,
        card_code = entry.card_code,
        event = entry.event or "UNKNOWN",
        result = entry.result,
        reason = entry.reason,
    })

end

function LockerDb.logs(lockerId, limit)

    limit = limit or 50

    if lockerId == nil then
        return Database.query(
            "SELECT id, locker_id, card_code, event, result, reason, created_at " ..
            "  FROM locker_logs ORDER BY id DESC LIMIT ?", { limit })
    end

    return Database.query(
        "SELECT id, locker_id, card_code, event, result, reason, created_at " ..
        "  FROM locker_logs WHERE locker_id = ? ORDER BY id DESC LIMIT ?", { lockerId, limit })

end

------------------------------------------------------------
-- Summary
------------------------------------------------------------

function LockerDb.summary()

    local counts = {
        total = Database.count(TABLE),
        [LockerDb.STATUS.EMPTY] = 0,
        [LockerDb.STATUS.ASSIGNED] = 0,
        [LockerDb.STATUS.EXPIRED] = 0,
        [LockerDb.STATUS.ERROR] = 0,
        open = Database.count(TABLE, "door_open = 1"),
        no_output = Database.count(TABLE, "output_register IS NULL OR output_bit IS NULL"),
    }

    for _, row in ipairs(Database.query("SELECT status, COUNT(*) AS n FROM " .. TABLE .. " GROUP BY status") or {}) do
        counts[row.status] = row.n
    end

    return counts

end

return LockerDb
