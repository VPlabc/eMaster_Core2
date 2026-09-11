-- tests/test_zk_reader.lua
-- Stage 1.4 (Plan section 21): the card reader.
--
-- Also the place to settle the CARD FORMAT. The controller reports the 24
-- Wiegand data bits; what is printed on the card is one bit wider. Every
-- formatting of the same swipe is printed below -- pick the one that matches
-- the number the server sends for that card and set reader.card_format to it.
-- Getting this wrong makes every card an unknown card.

local ok = pcall(require, "bootstrap")
if not ok then require("smartlocker.tests.bootstrap") end

local Config  = require("config")
local Harness = require("tests.harness")
local Reader  = require("hardware.zk_reader")

local WAIT_MS = 30000
local CARDS_WANTED = 3

Harness.start("Stage 1.4 -- ZK reader")

Harness.info("Source:  " .. tostring(Config.reader.source))
Harness.info("Reader:  " .. tostring(Config.reader.host) .. ":" .. tostring(Config.reader.port))
Harness.info("Format:  " .. tostring(Config.reader.card_format) ..
             (Config.reader.reconstruct_parity and " (parity reconstructed)" or ""))

------------------------------------------------------------
-- Connect
------------------------------------------------------------

Harness.section("Connection")

local connected, connectError = Reader.connect()

if not Harness.check("reader connected", connected, connectError) then

    if Config.reader.source ~= "card_api" then
        Harness.info("The zk_controller module is a Windows x86 build only. On another platform,")
        Harness.info("set reader.source to \"card_api\" and push cards in over POST /api/card/input.")
    end

    Harness.finish()
    return

end

Harness.check("isConnected() agrees", Reader.is_connected())

------------------------------------------------------------
-- Read cards
------------------------------------------------------------

Harness.section("Present " .. CARDS_WANTED .. " card(s) -- " .. (WAIT_MS / 1000) .. "s each")

local seen = {}

for index = 1, CARDS_WANTED do

    Harness.info("Waiting for card " .. index .. "...")

    local card, err = Reader.wait_card(WAIT_MS)

    if card == nil then

        Harness.skip("card " .. index, err or "no card presented")

    else

        seen[#seen + 1] = card

        Harness.check("card " .. index .. " read", card.card_code ~= nil and card.card_code ~= "")

        Harness.info("   card_code : " .. tostring(card.card_code))
        Harness.info("   raw       : " .. tostring(card.raw))
        Harness.info("   door      : " .. tostring(card.door) .. "   reader: " .. tostring(card.reader))
        Harness.info("   block     : " .. tostring(card.block_id))

        -- Every candidate format for the same swipe, so the right one can be
        -- recognised at a glance.
        local raw = card.raw
        local formats = { "raw", "decimal", "hex_msb", "hex_lsb" }
        local original = Config.reader.card_format

        for _, format in ipairs(formats) do
            Config.reader.card_format = format
            Harness.info(string.format("   as %-8s: %s", format, tostring(Reader.format_card(raw))))
        end

        Config.reader.card_format = original

        -- A code that is empty, or that still contains separators, will not
        -- match anything the server sends.
        Harness.check("card " .. index .. " format is usable",
                      card.card_code:match("^[%w]+$") ~= nil,
                      "codes are compared as trimmed upper case text")

    end

end

------------------------------------------------------------
-- De-bounce
------------------------------------------------------------

if #seen > 0 then

    Harness.section("Repeat suppression")

    Harness.info("Hold the same card against the reader for a few seconds.")

    local repeats = 0
    local elapsed = 0

    while elapsed < 6000 do

        Sleep(100)
        elapsed = elapsed + 100

        if Reader.poll() ~= nil then
            repeats = repeats + 1
        end

    end

    Harness.check("a held card is not re-read every poll", repeats <= 3,
                  repeats .. " read(s) in 6s, reader.repeat_ignore_ms = " ..
                  tostring(Config.reader.repeat_ignore_ms))

end

------------------------------------------------------------
-- Disconnect / reconnect
------------------------------------------------------------

Harness.section("Disconnect and reconnect")

Reader.disconnect()

Harness.check("disconnected", not Reader.is_connected())

local back, backError = Reader.connect()

Harness.check("reconnected", back, backError)

Harness.info("")
Harness.info("Checklist for Plan section 21:")
Harness.info("  [ ] the card_code matches what the server calls this card")
Harness.info("  [ ] DoorID identifies the cabinet (fill in reader.block_by_door for several)")
Harness.info("  [ ] pulling the network cable and plugging it back in recovers on its own")

Harness.finish()
