-- utils/time.lua
-- Clocks, timestamps and expiry arithmetic.
--
-- os.time() (whole seconds, local wall clock) is the only clock a script has --
-- os.clock() measures CPU time, not elapsed time, and would report a few
-- milliseconds after a 30 second door wait. Every timeout in this project is
-- specified in seconds for that reason (Plan sections 11 and 16); the one
-- millisecond-scale interval, the 100 ms unlock pulse, is produced with Sleep()
-- instead of measured.

local Time = {}

Time.SECONDS_PER_DAY = 86400

function Time.Now()
    return os.time()
end

-- Timestamp format used for every stored row and every log field. Local time,
-- because that is what the operator reading the Logs page is standing in.
function Time.Stamp(when)
    return os.date("%Y-%m-%d %H:%M:%S", when or os.time())
end

function Time.Date(when)
    return os.date("%Y-%m-%d", when or os.time())
end

------------------------------------------------------------
-- Parsing
------------------------------------------------------------

-- Accepts what the server realistically sends for expire_at:
--
--   "2026-12-31"                  date only
--   "2026-12-31 23:59:59"         date + time
--   "2026-12-31T23:59:59"         ISO 8601
--   "2026-12-31T23:59:59Z"        ISO 8601, UTC marker
--   "2026-12-31T23:59:59+07:00"   ISO 8601, offset
--   1798761599                    epoch seconds (number or numeric string)
--
-- Returns epoch seconds, plus a second value that is true when the input
-- carried no time-of-day. The caller needs that: a contractor whose card
-- expires "2026-12-31" is valid for the whole of the 31st, so a date-only value
-- has to be read as the end of that day rather than as midnight at its start
-- (see Time.ExpiryEpoch).
function Time.Parse(value)

    if value == nil then
        return nil
    end

    if type(value) == "number" then
        return math.tointeger(value) or math.floor(value), false
    end

    local text = tostring(value)

    text = text:match("^%s*(.-)%s*$")

    if text == "" then
        return nil
    end

    if text:match("^%-?%d+$") then
        return math.tointeger(tonumber(text)), false
    end

    local year, month, day = text:match("^(%d%d%d%d)-(%d%d)-(%d%d)")

    if not year then
        return nil
    end

    local hour, minute, second = text:match("^%d%d%d%d%-%d%d%-%d%d[T ](%d%d):(%d%d):?(%d*)")

    local dateOnly = hour == nil

    local epoch = os.time({
        year  = tonumber(year),
        month = tonumber(month),
        day   = tonumber(day),
        hour  = tonumber(hour) or 0,
        min   = tonumber(minute) or 0,
        sec   = tonumber(second) or 0,
        isdst = false,
    })

    if epoch == nil then
        return nil
    end

    -- A trailing Z or +hh:mm means the timestamp is not in this machine's zone;
    -- os.time() interpreted the fields as local, so undo that here. Without it a
    -- card expiring at "2026-12-31T17:00:00Z" would be honoured for another
    -- seven hours in Vietnam.
    local zone = text:match("([Zz])$") or text:match("([%+%-]%d%d:?%d%d)$")

    if zone and not dateOnly then

        local localOffset = Time.LocalUtcOffset()
        local zoneOffset = 0

        if zone ~= "Z" and zone ~= "z" then
            local sign, zh, zm = zone:match("^([%+%-])(%d%d):?(%d%d)$")
            zoneOffset = (tonumber(zh) * 3600 + tonumber(zm) * 60) * (sign == "-" and -1 or 1)
        end

        epoch = epoch + localOffset - zoneOffset

    end

    return epoch, dateOnly

end

-- Seconds this machine's local time is ahead of UTC.
function Time.LocalUtcOffset()

    local now = os.time()
    local utc = os.date("!*t", now)

    utc.isdst = false

    return os.difftime(now, os.time(utc))

end

------------------------------------------------------------
-- Expiry
------------------------------------------------------------

-- The instant a card stops being valid. `endOfDay` (the sync.expire_at_end_of_day
-- setting) decides how a date-only value is read; it defaults to true, which is
-- the reading a person expects from "expires 2026-12-31".
function Time.ExpiryEpoch(value, endOfDay)

    local epoch, dateOnly = Time.Parse(value)

    if epoch == nil then
        return nil
    end

    if dateOnly and endOfDay ~= false then
        epoch = epoch + Time.SECONDS_PER_DAY - 1
    end

    return epoch

end

-- nil/empty expire_at is NOT expired -- that is the normal state of an employee
-- record (Plan section 10). An unparseable value is treated as expired: a date
-- nobody can read must not silently become a permanent card.
function Time.IsExpired(value, now, endOfDay)

    if value == nil or value == "" then
        return false, nil
    end

    local epoch = Time.ExpiryEpoch(value, endOfDay)

    if epoch == nil then
        return true, nil
    end

    return (now or os.time()) > epoch, epoch

end

function Time.DaysUntil(value, now, endOfDay)

    local epoch = Time.ExpiryEpoch(value, endOfDay)

    if epoch == nil then
        return nil
    end

    return math.floor((epoch - (now or os.time())) / Time.SECONDS_PER_DAY)

end

------------------------------------------------------------
-- Elapsed-time helpers
------------------------------------------------------------

-- Deadline in the whole-second clock. Callers hold on to the value and ask
-- Time.Reached(deadline) each pass of the main loop, rather than blocking.
function Time.Deadline(seconds)
    return os.time() + (seconds or 0)
end

function Time.Reached(deadline)
    return deadline ~= nil and os.time() >= deadline
end

function Time.Since(epoch)

    if epoch == nil then
        return nil
    end

    return os.time() - epoch

end

return Time
