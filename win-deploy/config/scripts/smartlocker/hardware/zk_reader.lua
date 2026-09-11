-- hardware/zk_reader.lua
-- Card input (Plan sections 21 and 26). Two sources, one shape of result:
--
--   "zk"       the ZK access controller, through the gateway's in-process
--              zk_controller module. Realtime-log events arrive on a callback
--              that the engine delivers while the script is inside Sleep().
--   "card_api" an external reader POSTing to /api/card/input, landing in the
--              gateway's single-slot Card cache.
--   "both"     whichever produces a card first.
--
-- The controller reports the 24 Wiegand-26 data bits; the number printed on the
-- card is one bit wider, because it also carries the trailing parity bit. The
-- conversion below is the one verified against real cards in the main gateway's
-- zkcard.lua -- reported 3690153 (0x384EA9) is card 0x709D52.
--
-- No business logic here: this module never decides whether a card may open
-- anything, only what card was presented and at which reader.

local zk = nil

do
    -- The PullSDK module only exists on Windows x86 builds. A missing reader
    -- must leave the rest of the application (database, sync, frontend, PLC
    -- tests) working, so this is a soft failure reported at connect time.
    local ok, module = pcall(require, "zk_controller")
    zk = ok and module or nil
end

local Config = require("config")
local Logger = require("utils.logger")
local Time   = require("utils.time")

local Reader = {}

local settings = Config.reader

local started = false
local pending = nil
local lastCard = nil
local lastCardAt = 0
local lastSeen = {}      -- card_code -> os.time() of the last accepted swipe
local connected = false

------------------------------------------------------------
-- Card number conversion
------------------------------------------------------------

local function countOnes(value)

    local ones = 0

    while value > 0 do
        ones = ones + (value & 1)
        value = value >> 1
    end

    return ones

end

-- Wiegand-26's trailing parity is odd over the low 12 data bits.
local function trailingOddParity(data24)
    return (countOnes(data24 & 0xFFF) % 2 == 1) and 0 or 1
end

local function toHexPair(value)

    local digits = (value <= 0xFFFFFF) and 6 or 8
    local msb = string.format("%0" .. digits .. "X", value)

    local lsb = ""

    for i = #msb - 1, 1, -2 do
        lsb = lsb .. msb:sub(i, i + 1)
    end

    return msb, lsb

end

-- Raw reader value -> the card_code stored in the database, per
-- reader.card_format. Returns nil when the value cannot be converted in the
-- configured format, which is a card to ignore rather than an error.
function Reader.format_card(raw)

    local format = settings.card_format or "hex_msb"

    if format == "raw" then
        return raw ~= nil and tostring(raw) or nil
    end

    local number = math.tointeger(tonumber(raw))

    if number == nil or number < 0 then
        -- Not numeric: hex formats have nothing to convert, so the value is
        -- passed through as text -- a reader that already reports "ABCD0123"
        -- then works with no configuration change.
        if raw == nil then
            return nil
        end
        return tostring(raw):upper()
    end

    if format == "decimal" then
        return tostring(number)
    end

    local trueValue = (number << 1) | (settings.reconstruct_parity and trailingOddParity(number) or 0)
    local msb, lsb = toHexPair(trueValue)

    if format == "hex_lsb" then
        return lsb
    end

    return msb

end

------------------------------------------------------------
-- Connection
------------------------------------------------------------

local function blockForDoor(doorId)

    local map = settings.block_by_door or {}
    local key = tostring(doorId or "")

    return map[key] or settings.default_block or 1

end

function Reader.connect()

    if settings.source == "card_api" then
        connected = true
        return true
    end

    if zk == nil then
        connected = false
        return false, "zk_controller module is not available in this build (Windows x86 only)"
    end

    if started then
        return true
    end

    zk.onCard(function(event)
        pending = event
    end)

    zk.onConnectionChanged(function(isConnected)

        connected = isConnected == true

        if connected then
            Logger.info("ZK reader connected")
        else
            Logger.reader_error("ZK reader disconnected", "onConnectionChanged")
        end

    end)

    zk.setAutoReconnect(true)

    local ok, err = zk.connect(settings.host, settings.port, settings.timeout_ms, settings.password or "")

    if not ok then
        connected = false
        return false, tostring(err)
    end

    zk.startRTLog()

    started = true
    connected = true

    Reader.flush()

    return true

end

-- Throws away whatever the controller had buffered. It keeps up to 30 realtime
-- events and replays the backlog on the first poll after connecting -- without
-- this, a swipe from yesterday opens a locker the moment the gateway starts.
function Reader.flush()

    local elapsed = 0
    local step = 10

    while elapsed < (settings.flush_ms or 1500) do
        Sleep(step)   -- Sleep() is also what delivers the queued zk callbacks
        elapsed = elapsed + step
    end

    pending = nil

    if type(Card) == "table" and type(Card.Clear) == "function" then
        Card.Clear()
    end

end

function Reader.disconnect()

    if zk ~= nil and started then
        zk.stopRTLog()
        zk.disconnect()
    end

    started = false
    connected = false

    return true

end

function Reader.is_connected()

    if settings.source == "card_api" then
        return true
    end

    if zk == nil then
        return false
    end

    return zk.isConnected() == true

end

function Reader.last_error()

    if zk == nil then
        return "zk_controller module is not available"
    end

    return zk.lastError()

end

------------------------------------------------------------
-- Reading cards
------------------------------------------------------------

local function fromZkEvent(event)

    local code = Reader.format_card(event.CardData)

    if code == nil then
        return nil
    end

    return {
        card_code = code,
        raw = tostring(event.CardData),
        source = "zk",
        door = event.DoorID,
        reader = event.ReaderID,
        block_id = blockForDoor(event.DoorID),
        at = Time.Now(),
    }

end

local function fromCardCache()

    if type(Card) ~= "table" or type(Card.Available) ~= "function" then
        return nil
    end

    if not Card.Available() then
        return nil
    end

    local entry = Card.Get()

    Card.Clear()

    if entry == nil or entry.uid == nil or entry.uid == "" then
        return nil
    end

    return {
        card_code = tostring(entry.uid):upper(),
        raw = tostring(entry.uid),
        source = "card_api",
        -- The card API reports a Position rather than a door; it maps onto the
        -- same block lookup so a desktop reader can be told which cabinet it
        -- stands at.
        door = entry.position,
        block_id = blockForDoor(entry.position),
        at = Time.Now(),
    }

end

-- Non-blocking: returns a card table, or nil when nothing has been presented.
-- This is what the main loop calls; a swipe repeated within
-- reader.repeat_ignore_ms is swallowed here, so a card held against the reader
-- does not unlock the door over and over while the person is opening it.
function Reader.poll()

    local card = nil
    local source = settings.source or "zk"

    if (source == "zk" or source == "both") and pending ~= nil then
        local event = pending
        pending = nil
        card = fromZkEvent(event)
    end

    if card == nil and (source == "card_api" or source == "both") then
        card = fromCardCache()
    end

    if card == nil then
        return nil
    end

    local now = Time.Now()
    local ignoreSeconds = math.max(1, math.floor((settings.repeat_ignore_ms or 3000) / 1000))
    local previous = lastSeen[card.card_code]

    if previous and (now - previous) < ignoreSeconds then
        return nil
    end

    lastSeen[card.card_code] = now
    lastCard = card
    lastCardAt = now

    return card

end

-- Blocking wait, for the hardware tests and for scripted flows. The main
-- application never uses it: while it waits, nothing else in the script runs --
-- no door timers, no scheduled sync, no RabbitMQ.
function Reader.wait_card(timeoutMs)

    local ok, err = Reader.connect()

    if not ok then
        return nil, err
    end

    local elapsed = 0
    local step = math.max(10, settings.poll_interval_ms or 50)

    -- Only swipes from this moment count.
    pending = nil

    while elapsed < (timeoutMs or 10000) do

        Sleep(step)
        elapsed = elapsed + step

        local card = Reader.poll()

        if card ~= nil then
            return card
        end

    end

    return nil, "timeout"

end

function Reader.get_card_code()
    return lastCard and lastCard.card_code or nil
end

function Reader.last_card()
    return lastCard
end

function Reader.clear()
    pending = nil
    lastSeen = {}
end

function Reader.status()

    return {
        source = settings.source,
        connected = Reader.is_connected(),
        started = started,
        last_card = lastCard and lastCard.card_code or nil,
        last_card_at = lastCardAt > 0 and Time.Stamp(lastCardAt) or nil,
        module_available = zk ~= nil,
    }

end

return Reader
