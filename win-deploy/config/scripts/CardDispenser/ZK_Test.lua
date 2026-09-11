-- ZK_Test.lua
local zk = require("zk_controller")

local WAIT_MS = 60000
local POLL_MS = 200

-- RTLog reports only the 24 Wiegand-26 DATA bits (8-bit facility code +
-- 16-bit card number). The number printed on the card -- and what other
-- systems display -- is one bit wider: it also carries the trailing
-- odd-parity bit. So the reported value comes out at exactly half the true
-- one. Verified against a real card: reported 3690153 (0x384EA9) -> true
-- 0x709D52.
--
-- That trailing bit is odd parity over the low 12 data bits, so it's
-- computed rather than guessed.
--
-- CAVEAT: on the one card tested so far that parity worked out to 0, which
-- means "shift left 1" and "shift left 1, then OR in the parity" both happen
-- to give the right answer. They only diverge on a card whose parity is 1.
-- Swipe a second card to confirm which rule actually holds; if the parity
-- version turns out wrong, set this to false to just append a 0 instead.
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

-- Returns: trueMsb, trueLsb, rawMsb  (all nil if CardData isn't numeric)
local function CardHex(cardData)
    local n = math.tointeger(tonumber(cardData))
    if n == nil or n < 0 then
        return nil, nil, nil
    end

    local trueValue = (n << 1) | (RECONSTRUCT_PARITY and TrailingOddParity(n) or 0)

    local trueMsb, trueLsb = ToHexPair(trueValue)
    local rawMsb = select(1, ToHexPair(n))

    return trueMsb, trueLsb, rawMsb
end

local cardCount = 0
local lastEvent = nil

zk.onCard(function(event)
    cardCount = cardCount + 1
    lastEvent = event

    local msb, lsb, rawMsb = CardHex(event.CardData)

    Log.Info("watch_card: CARD EVENT #" .. cardCount ..
        " CardData=" .. tostring(event.CardData) ..
        " HEX[MSB]=0x" .. tostring(msb) ..
        " HEX[LSB]=0x" .. tostring(lsb) ..
        " (raw24=0x" .. tostring(rawMsb) .. ")" ..
        " DoorID=" .. tostring(event.DoorID) ..
        " ReaderID=" .. tostring(event.ReaderID) ..
        " InOutStatus=" .. tostring(event.InOutStatus) ..
        " VerifyMode=" .. tostring(event.VerifyMode) ..
        " EventType=" .. tostring(event.EventType) ..
        " Timestamp=" .. tostring(event.Timestamp))
end)

Log.Info("watch_card: connecting to 192.168.1.201:4370 ...")
local ok, err = zk.connect("192.168.1.201", 4370, 2000, "")

if not ok then
    Log.Error("watch_card: connect failed: " .. tostring(err))
    return
end

zk.startRTLog()
Log.Info("watch_card: connected, RTLog started -- present a card now (waiting up to " .. WAIT_MS .. "ms)")

local elapsed = 0
while elapsed < WAIT_MS do
    Sleep(POLL_MS)
    elapsed = elapsed + POLL_MS
end

Log.Info("watch_card: done waiting -- " .. cardCount .. " card event(s) received")
