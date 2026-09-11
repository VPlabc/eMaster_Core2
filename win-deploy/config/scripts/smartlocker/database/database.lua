-- database/database.lua
-- The local store: SQLite, through the gateway's Db.* binding.
--
-- Plan section 15 asks for SQLite, and the gateway now provides it: Db.Open /
-- Db.Exec / Db.Query / Db.Begin / Db.Commit / Db.Rollback, backed by one
-- connection per script (src/SqlDatabase.cpp). This module owns the schema and
-- hands the rest of the application a small set of typed helpers, so no other
-- file writes SQL for the two mutable tables.
--
-- WHY THE WEB UI CAN READ THIS FILE. The connection is opened WAL, and the
-- gateway's locker dashboard opens the same path read-only on its own
-- connection. That is the whole integration: this script writes, the UI reads,
-- and nothing in the web layer is allowed to change a locker row -- an action
-- from the dashboard comes back here as an HTTP route (api/frontend.lua) so the
-- state machine that owns the door is the thing that moves it.
--
-- Four tables, exactly the ones section 15 lists:
--   employees     one row per card
--   lockers       one row per physical door, including its PLC mapping
--   locker_logs   the access/door audit trail the UI shows
--   system_logs   gateway-level events worth keeping next to them

local Config = require("config")
local Logger = require("utils.logger")
local Time   = require("utils.time")

local Database = {}

-- Binds SQL NULL. Not the same thing as Lua nil: nil in a parameter array ends
-- the array, and nil in an update table means "don't touch this column".
Database.NULL = Db.NULL

-- 2 added the contractor daily-usage columns to `employees` (CardScanPlan
-- section 1). Existing databases are brought forward by ensureColumns() below
-- rather than by a version branch, so the two paths -- fresh CREATE TABLE and
-- ALTER on an installed cabinet -- cannot describe different tables.
local SCHEMA_VERSION = 2

local opened = false

------------------------------------------------------------
-- Schema
------------------------------------------------------------

-- Written out rather than generated: this is the contract the web UI's queries
-- (src/WebServer.cpp, BuildLockerStateJson) are written against, and a schema
-- assembled at runtime would let the two drift apart silently.
local SCHEMA = {

    [[CREATE TABLE IF NOT EXISTS meta (
        key   TEXT PRIMARY KEY,
        value TEXT
    )]],

    [[CREATE TABLE IF NOT EXISTS employees (
        id         INTEGER PRIMARY KEY AUTOINCREMENT,
        username   TEXT    NOT NULL DEFAULT '',
        card_code  TEXT    NOT NULL UNIQUE,
        role       TEXT    NOT NULL DEFAULT 'employee',
        gender     TEXT,
        expire_at  TEXT,
        active     INTEGER NOT NULL DEFAULT 1,
        locker_id  INTEGER,
        -- Contractor daily usage (CardScanPlan section 1). The plan names these
        -- contractor_start_date / usage_date / usage_started_at /
        -- usage_last_open_at / usage_completed_at / daily_usage_status; they are
        -- spelled the way the rest of this table is (expire_at, last_open_at)
        -- so a reader does not have to keep two conventions in their head.
        -- contractor_expire_date is the existing `expire_at` -- there is no
        -- second expiry column, because two of them would eventually disagree.
        start_at           TEXT,
        usage_date         TEXT,
        usage_started_at   TEXT,
        usage_last_open_at TEXT,
        usage_completed_at TEXT,
        usage_status       TEXT NOT NULL DEFAULT 'NOT_STARTED',
        created_at TEXT    NOT NULL,
        updated_at TEXT    NOT NULL
    )]],

    [[CREATE TABLE IF NOT EXISTS lockers (
        id              INTEGER PRIMARY KEY AUTOINCREMENT,
        block_id        INTEGER NOT NULL,
        block_name      TEXT,
        locker_number   INTEGER NOT NULL,
        locker_type     TEXT    NOT NULL DEFAULT 'employee',
        input_register  INTEGER,
        input_bit       INTEGER,
        output_register INTEGER,
        output_bit      INTEGER,
        status          TEXT    NOT NULL DEFAULT 'EMPTY',
        card_code       TEXT,
        assigned_at     TEXT,
        expire_at       TEXT,
        last_open_at    TEXT,
        last_close_at   TEXT,
        last_error      TEXT,
        -- Door sensor and state-machine phase, mirrored here for the UI. They
        -- are runtime facts, not stored state: core/locker.lua writes them only
        -- when they CHANGE, and restart recovery re-reads the doors from the
        -- PLC rather than believing what is in this column.
        door_open       INTEGER NOT NULL DEFAULT 0,
        runtime_state   TEXT    NOT NULL DEFAULT 'IDLE',
        created_at      TEXT    NOT NULL,
        updated_at      TEXT    NOT NULL,
        UNIQUE(block_id, locker_number)
    )]],

    [[CREATE TABLE IF NOT EXISTS locker_logs (
        id         INTEGER PRIMARY KEY AUTOINCREMENT,
        locker_id  INTEGER,
        card_code  TEXT,
        event      TEXT NOT NULL,
        result     TEXT,
        reason     TEXT,
        created_at TEXT NOT NULL
    )]],

    [[CREATE TABLE IF NOT EXISTS system_logs (
        id         INTEGER PRIMARY KEY AUTOINCREMENT,
        level      TEXT NOT NULL DEFAULT 'INFO',
        type       TEXT,
        message    TEXT,
        data_json  TEXT,
        created_at TEXT NOT NULL
    )]],

    -- The three lookups the access path makes on every swipe, plus the two the
    -- dashboard makes on every refresh. Without them a 400-card table is a
    -- full scan per card read.
    [[CREATE INDEX IF NOT EXISTS idx_employees_card ON employees(card_code)]],
    [[CREATE INDEX IF NOT EXISTS idx_employees_locker ON employees(locker_id)]],
    [[CREATE INDEX IF NOT EXISTS idx_lockers_card ON lockers(card_code)]],
    [[CREATE INDEX IF NOT EXISTS idx_lockers_status ON lockers(status)]],
    [[CREATE INDEX IF NOT EXISTS idx_locker_logs_locker ON locker_logs(locker_id, id DESC)]],
    [[CREATE INDEX IF NOT EXISTS idx_locker_logs_created ON locker_logs(created_at)]],
}

------------------------------------------------------------
-- Migrations
------------------------------------------------------------

-- Columns added after a cabinet was already installed. CREATE TABLE IF NOT
-- EXISTS does nothing to a table that already exists, so a database built by an
-- older build would keep its old shape and every read of the new columns would
-- come back nil -- which for usage_status is indistinguishable from "this
-- contractor has never used their locker".
--
-- Written as "add it if PRAGMA table_info does not list it" rather than as a
-- numbered migration step: it is idempotent, it runs identically on a fresh
-- database (where the CREATE above already made the column) and on an upgraded
-- one, and there is no version number to forget to bump.
local ADDED_COLUMNS = {

    employees = {
        { name = "start_at",           decl = "TEXT" },
        { name = "usage_date",         decl = "TEXT" },
        { name = "usage_started_at",   decl = "TEXT" },
        { name = "usage_last_open_at", decl = "TEXT" },
        { name = "usage_completed_at", decl = "TEXT" },
        { name = "usage_status",       decl = "TEXT NOT NULL DEFAULT 'NOT_STARTED'" },
    },

}

local function ensureColumns()

    for table_name, columns in pairs(ADDED_COLUMNS) do

        local present = {}

        for _, row in ipairs(Db.Query("PRAGMA table_info(" .. table_name .. ")") or {}) do
            present[row.name] = true
        end

        -- An empty PRAGMA means the table is not there at all, which cannot
        -- happen after the schema ran -- but adding columns to nothing would
        -- log an SQL error per column, so say so once instead.
        if next(present) == nil then
            Logger.warning("cannot inspect table " .. table_name .. "; skipping column checks")
        else

            for _, column in ipairs(columns) do

                if not present[column.name] then

                    local changes, err = Db.Exec("ALTER TABLE " .. table_name ..
                                                 " ADD COLUMN " .. column.name .. " " .. column.decl)

                    if changes == nil then
                        return false, "adding " .. table_name .. "." .. column.name .. ": " .. tostring(err)
                    end

                    Logger.info("database upgrade: added column " .. table_name .. "." .. column.name)

                end

            end

        end

    end

    return true

end

------------------------------------------------------------
-- Open
------------------------------------------------------------

-- Database.open([path]) -> true | false, error
--
-- `path` is relative to the gateway's CONFIG directory (where config.db and
-- logs.db live), which is also where the web UI's web.locker_db_path resolves
-- from -- so the two halves point at the same file by default with nothing to
-- configure.
function Database.open(path)

    path = path or Config.database.path or "smartlocker.db"

    local ok, err = Db.Open(path)

    if not ok then
        return false, "cannot open " .. tostring(path) .. ": " .. tostring(err)
    end

    opened = true

    for _, statement in ipairs(SCHEMA) do
        local changes, execErr = Db.Exec(statement)
        if changes == nil then
            opened = false
            return false, "schema: " .. tostring(execErr)
        end
    end

    local upgraded, upgradeErr = ensureColumns()

    if not upgraded then
        opened = false
        return false, "schema: " .. tostring(upgradeErr)
    end

    -- Records which schema this database was built with. Written after the
    -- columns are actually there, so an interrupted upgrade is retried on the
    -- next start rather than skipped because the number already said 2.
    local version = Db.Scalar("SELECT value FROM meta WHERE key = 'schema_version'")

    if version == nil then
        Db.Exec("INSERT INTO meta(key, value) VALUES('schema_version', ?)", { tostring(SCHEMA_VERSION) })
    elseif (tonumber(version) or 0) < SCHEMA_VERSION then

        Logger.info("database schema upgraded from version " .. tostring(version) ..
                    " to " .. SCHEMA_VERSION)

        Db.Exec("UPDATE meta SET value = ? WHERE key = 'schema_version'", { tostring(SCHEMA_VERSION) })

    elseif tonumber(version) ~= SCHEMA_VERSION then

        -- A database written by a NEWER build. Left exactly as it is: this
        -- build's columns are all present (ensureColumns just checked), and
        -- stamping the number down would hide the mismatch from the build that
        -- actually owns the extra tables.
        Logger.warning("database schema version is " .. tostring(version) ..
                       ", this build expects " .. SCHEMA_VERSION)

    end

    local employees = Db.Scalar("SELECT COUNT(*) FROM employees") or 0
    local lockers = Db.Scalar("SELECT COUNT(*) FROM lockers") or 0

    Logger.info("opened database " .. tostring(Db.Path()) ..
                " (" .. employees .. " employees, " .. lockers .. " lockers)")

    return true

end

function Database.is_open()
    return opened and Db.IsOpen()
end

function Database.path()
    return Db.Path()
end

function Database.close()
    Db.Close()
    opened = false
end

------------------------------------------------------------
-- Statements
------------------------------------------------------------

-- All four wrap the native call for one reason: a failed statement is logged
-- once, here, with the SQL that caused it. A caller that ignores the error
-- still leaves a trace in the runtime log.
local function fail(what, sql, err)
    Logger.error("SQL " .. what .. " failed: " .. tostring(err) .. " -- " .. tostring(sql))
    return nil, err
end

function Database.exec(sql, params)

    local changes, lastId = Db.Exec(sql, params)

    if changes == nil then
        return fail("exec", sql, lastId)
    end

    return changes, lastId

end

function Database.query(sql, params)

    local rows, err = Db.Query(sql, params)

    if rows == nil then
        return fail("query", sql, err)
    end

    return rows

end

function Database.query_one(sql, params)

    local row, err = Db.QueryOne(sql, params)

    if row == nil and err ~= nil then
        return fail("query", sql, err)
    end

    return row

end

function Database.scalar(sql, params)

    local value, err = Db.Scalar(sql, params)

    if value == nil and err ~= nil then
        return fail("query", sql, err)
    end

    return value

end

------------------------------------------------------------
-- Generic row helpers
------------------------------------------------------------

-- Column whitelists. Every insert and update goes through them, so a typo in a
-- field name is a Lua-side error rather than SQLite silently rejecting the
-- statement -- and no caller can interpolate a column name of its own.
local COLUMNS = {

    employees = {
        "username", "card_code", "role", "gender", "expire_at",
        "active", "locker_id", "created_at", "updated_at",
        "start_at", "usage_date", "usage_started_at", "usage_last_open_at",
        "usage_completed_at", "usage_status",
    },

    lockers = {
        "block_id", "block_name", "locker_number", "locker_type",
        "input_register", "input_bit", "output_register", "output_bit",
        "status", "card_code", "assigned_at", "expire_at",
        "last_open_at", "last_close_at", "last_error",
        "door_open", "runtime_state", "created_at", "updated_at",
    },

    locker_logs = { "locker_id", "card_code", "event", "result", "reason", "created_at" },
    system_logs = { "level", "type", "message", "data_json", "created_at" },

}

local function allowed(table_name, column)

    local list = COLUMNS[table_name]

    if list == nil then
        return false
    end

    for _, name in ipairs(list) do
        if name == column then
            return true
        end
    end

    return false

end

-- SQLite has no boolean type. true/false are stored as 1/0 by the binding
-- already; this is for the read direction, where a caller wants a boolean back.
function Database.to_boolean(value)
    return value == 1 or value == true or value == "1"
end

-- Database.insert("employees", { ... }) -> id | nil, error
function Database.insert(table_name, row)

    local columns, placeholders, params = {}, {}, {}

    row.created_at = row.created_at or Time.Stamp()
    row.updated_at = Time.Stamp()

    for _, column in ipairs(COLUMNS[table_name] or {}) do

        local value = row[column]

        if value ~= nil then
            columns[#columns + 1] = column
            placeholders[#placeholders + 1] = "?"
            params[#params + 1] = (value == Database.NULL) and Database.NULL or value
        end

    end

    if #columns == 0 then
        return nil, "nothing to insert into " .. tostring(table_name)
    end

    local sql = "INSERT INTO " .. table_name .. " (" .. table.concat(columns, ", ") ..
                ") VALUES (" .. table.concat(placeholders, ", ") .. ")"

    local changes, lastId = Database.exec(sql, params)

    if changes == nil then
        return nil, lastId
    end

    return lastId

end

-- Database.update("lockers", id, { status = "EMPTY", card_code = Database.NULL })
--
-- A field set to Database.NULL clears the column; a field left out is not
-- touched. That distinction is the whole reason for the sentinel.
function Database.update(table_name, id, fields)

    local assignments, params = {}, {}

    for column, value in pairs(fields) do

        if allowed(table_name, column) and column ~= "created_at" then
            assignments[#assignments + 1] = column .. " = ?"
            params[#params + 1] = value
        elseif not allowed(table_name, column) then
            Logger.warning("update " .. table_name .. ": ignoring unknown column '" .. tostring(column) .. "'")
        end

    end

    if #assignments == 0 then
        return 0
    end

    assignments[#assignments + 1] = "updated_at = ?"
    params[#params + 1] = Time.Stamp()

    params[#params + 1] = id

    local sql = "UPDATE " .. table_name .. " SET " .. table.concat(assignments, ", ") .. " WHERE id = ?"

    return Database.exec(sql, params)

end

function Database.delete(table_name, id)
    return Database.exec("DELETE FROM " .. table_name .. " WHERE id = ?", { id })
end

function Database.count(table_name, where, params)

    local sql = "SELECT COUNT(*) FROM " .. table_name

    if where then
        sql = sql .. " WHERE " .. where
    end

    return Database.scalar(sql, params) or 0

end

------------------------------------------------------------
-- Meta
------------------------------------------------------------

-- One-line key/value settings that belong to this DATABASE rather than to the
-- configuration file: things the running system decided, not things an
-- installer typed. The overtime end-time the server pushes is the only one so
-- far -- it has to survive a restart in the middle of an extended shift, and it
-- must not end up written back into smartlocker.json where tomorrow would
-- inherit tonight's overtime.
function Database.meta_get(key, fallback)

    local value = Database.scalar("SELECT value FROM meta WHERE key = ?", { key })

    if value == nil then
        return fallback
    end

    return value

end

function Database.meta_set(key, value)

    if value == nil then
        return Database.exec("DELETE FROM meta WHERE key = ?", { key })
    end

    return Database.exec("INSERT INTO meta(key, value) VALUES(?, ?) " ..
                         "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                         { key, tostring(value) })

end

------------------------------------------------------------
-- Transactions
------------------------------------------------------------

function Database.begin()
    return Db.Begin()
end

function Database.commit()
    return Db.Commit()
end

function Database.rollback()
    return Db.Rollback()
end

function Database.in_transaction()
    return Db.InTransaction() > 0
end

-- Database.transaction_do(function() ... return true end) -> ok, result|error
--
-- Commits when the body returns anything but false/nil, rolls back when it
-- returns false or raises. Every assignment and release in this application
-- goes through here: the employee row and the locker row are two halves of one
-- fact (Plan section 15's closing rule), and half of it committed is a locker
-- that belongs to nobody while its owner believes otherwise.
function Database.transaction_do(fn)

    local started, beginErr = Db.Begin()

    if not started then
        return false, tostring(beginErr)
    end

    local ok, result = pcall(fn)

    if not ok then
        Db.Rollback()
        return false, tostring(result)
    end

    if result == false or result == nil then
        Db.Rollback()
        return false, "transaction body reported failure"
    end

    local committed, commitErr = Db.Commit()

    if not committed then
        return false, tostring(commitErr)
    end

    return true, result

end

------------------------------------------------------------
-- Maintenance
------------------------------------------------------------

function Database.stats()

    if not Database.is_open() then
        return { open = false }
    end

    return {
        open = true,
        path = Db.Path(),
        employees = Database.count("employees"),
        lockers = Database.count("lockers"),
        locker_logs = Database.count("locker_logs"),
        in_transaction = Database.in_transaction(),
    }

end

-- Deletes audit rows older than `days`. The log tables are the only ones that
-- grow without bound, and a cabinet doing 200 swipes a day fills them steadily.
-- 0 keeps everything.
function Database.prune_logs(days)

    if days == nil or days <= 0 then
        return 0
    end

    local cutoff = os.date("%Y-%m-%d %H:%M:%S", os.time() - days * 86400)

    local removed = Database.exec("DELETE FROM locker_logs WHERE created_at < ?", { cutoff }) or 0
    removed = removed + (Database.exec("DELETE FROM system_logs WHERE created_at < ?", { cutoff }) or 0)

    if removed > 0 then
        Logger.info("pruned " .. removed .. " log row(s) older than " .. days .. " days")
    end

    return removed

end

-- Test support: empties every table and resets the id sequences. Never called
-- by the application.
function Database.reset()

    -- sqlite_sequence only exists once an AUTOINCREMENT table has had a row,
    -- so on a brand-new database it is legitimately missing -- and deleting
    -- from it would log an SQL error on every first run.
    local hasSequence = (Database.scalar(
        "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = 'sqlite_sequence'") or 0) > 0

    for _, name in ipairs({ "locker_logs", "system_logs", "lockers", "employees" }) do

        Database.exec("DELETE FROM " .. name)

        if hasSequence then
            Database.exec("DELETE FROM sqlite_sequence WHERE name = ?", { name })
        end

    end

    -- schema_version stays: it describes the file, not its contents, and
    -- deleting it would have the next open re-insert it anyway.
    Database.exec("DELETE FROM meta WHERE key <> 'schema_version'")

    return true

end

return Database
