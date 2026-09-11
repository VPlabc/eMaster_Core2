-- zkcard.lua
-- Reads the card at the scan position via the ZK access controller
-- (in-process zk_controller module -- no bridge process). Same role as
-- card.lua, but for the ZK reader instead of the REST Card Client API.
-- No PLC logic and no business logic in this module.

local Config = require("config")

local zk = require("zk_controller")

local ZkCard = {}

local POLL_MS = 100

-- The controller buffers up to 30 realtime events and hands the whole
-- backlog over on the first poll after connecting, so a swipe from hours ago
-- would otherwise look like the card we're waiting for. Long enough to cover
-- several 500ms poll cycles.
local FLUSH_MS = 1500

local started = false
local pending = nil

------------------------------------------------------------
-- Card number conversion
------------------------------------------------------------

-- RTLog reports only the 24 Wiegand-26 DATA bits (8-bit facility code +
-- 16-bit card number). The number printed on the card -- and what other
-- systems display -- is one bit wider: it also carries the trailing
-- odd-parity bit, so the reported value comes out at exactly half the true
-- one. Verified against a real card: reported 3690153 (0x384EA9) -> true
-- 0x709D52.
--
-- CAVEAT: on the card tested, that parity bit worked out to 0, so "append a
-- 0" and "append the computed parity" both produce the right answer. They
-- only diverge on a card whose parity is 1. If a card ever reads back off by
-- exactly 1, set this to false.
local RECONSTRUCT_PARITY = true

local function CountOnes(v)
    local ones = 0
    while v > 0 do
        ones = ones + (v & 1)
        v = v >> 1
    end
    return ones
end

-- Wiegand-26's trailing parity is odd over the low 12 data bits: the bit is
-- whatever keeps that total count odd.
local function TrailingOddParity(data24)
    return (CountOnes(data24 & 0xFFF) % 2 == 1) and 0 or 1
end

local function ToHexPair(value)
    -- 6 digits = 24 bits, the width of a Wiegand-26 credential; anything
    -- wider widens to 32 bits so no significant byte is truncated.
    local digits = (value <= 0xFFFFFF) and 6 or 8
    local msb = string.format("%0" .. digits .. "X", value)

    local lsb = ""
    for i = #msb - 1, 1, -2 do
        lsb = lsb .. msb:sub(i, i + 1)
    end

    return msb, lsb
end

-- Returns: trueMsb, trueLsb, raw24Msb  (all nil if cardData isn't numeric)
function ZkCard.CardHex(cardData)

    local n = math.tointeger(tonumber(cardData))

    if n == nil or n < 0 then
        return nil, nil, nil
    end

    local trueValue = (n << 1) | (RECONSTRUCT_PARITY and TrailingOddParity(n) or 0)

    local trueMsb, trueLsb = ToHexPair(trueValue)
    local raw24Msb = select(1, ToHexPair(n))

    return trueMsb, trueLsb, raw24Msb

end

------------------------------------------------------------
-- Connection lifecycle
------------------------------------------------------------

function ZkCard.Start()

    if started then
        return true
    end

    zk.onCard(function(event)
        pending = event
    end)

    zk.setAutoReconnect(true)

    local ok, err = zk.connect(Config.ZK_IP, Config.ZK_PORT, Config.ZK_TIMEOUT, "")

    if not ok then
        return false, err
    end

    zk.startRTLog()
    started = true

    ZkCard.Flush()

    return true

end

-- Consumes (and discards) whatever the controller had buffered, so only
-- swipes from this point on count.
function ZkCard.Flush()

    local elapsed = 0

    while elapsed < FLUSH_MS do
        Sleep(POLL_MS)   -- Sleep() is also what delivers queued zk callbacks
        elapsed = elapsed + POLL_MS
    end

    pending = nil

end

function ZkCard.IsConnected()
    return zk.isConnected()
end

function ZkCard.Clear()
    pending = nil
end

------------------------------------------------------------
-- Wait for a card at the scan position
------------------------------------------------------------

function ZkCard.WaitCard(timeoutMs)

    local ok, err = ZkCard.Start()

    if not ok then
        return { ok = false, error = "ZK connect failed: " .. tostring(err) }
    end

    -- Only a swipe that happens from here on counts -- anything still
    -- pending predates this scan cycle.
    pending = nil

    local elapsed = 0

    while elapsed < timeoutMs do

        Sleep(POLL_MS)
        elapsed = elapsed + POLL_MS

        if pending ~= nil then

            local event = pending
            pending = nil

            local msb, lsb, raw24 = ZkCard.CardHex(event.CardData)

            if msb ~= nil then
                return {
                    ok = true,
                    uid = msb,              -- MSB hex, the value printed on the card
                    uid_lsb = lsb,
                    raw = tostring(event.CardData),
                    raw_hex = raw24,
                    door = event.DoorID,
                    reader = event.ReaderID,
                }
            end

            -- Non-numeric CardData: not something we can convert, so ignore
            -- it and keep waiting out the remaining timeout.

        end

    end

    return { ok = false }

end

return ZkCard
