-- led.lua
-- led.lua
-- TDM-800 LED display over Serial2 (RS232). See request/LEDBoard.md for the
-- frame layout. This is the ONLY module that builds LED protocol frames --
-- business logic calls Led.Show()/ShowError()/... and never touches bytes.

local Led = {}

------------------------------------------------------------
-- Protocol constants (request/LEDBoard.md "Recommended Default Frame")
------------------------------------------------------------

local CMD       = 0x00   -- update the display immediately
local ADDR      = 0x30   -- broadcast to all displays (0x31..0x40 = individual)
local SNUM      = 0x31   -- one scene
local SPEED     = 0x30   -- scroll in, then stop and stay visible
local DIRECTION = 0x30   -- static display (0x30 = right-to-left scroll)
local COLOR     = 0x30   -- red -- the only colour this hardware supports
local TERMINATOR = 0x0D  -- without this the controller keeps waiting and never updates

local opened = false

------------------------------------------------------------
-- Text encoding
------------------------------------------------------------

-- The controller is ASCII-only, so Vietnamese has to go out without
-- diacritics. Messages in messages.lua are already written that way; this is
-- a backstop for anything built at runtime (employee names off the citizen
-- card, mainly, which do carry diacritics).
-- Both cases are spelled out explicitly rather than deriving uppercase at
-- runtime: these are multi-byte UTF-8 sequences, and Lua's string.upper()
-- only touches ASCII bytes, so "ễ":upper() is still "ễ" -- the uppercase
-- forms would never match and would then be stripped as high bytes, turning
-- "NGUYỄN VĂN A" into "NGUYN VN A". Names on citizen cards are uppercase, so
-- that path is the common one, not the edge case.
local DIACRITICS = {
    ["à"]="a", ["á"]="a", ["ạ"]="a", ["ả"]="a", ["ã"]="a",
    ["â"]="a", ["ầ"]="a", ["ấ"]="a", ["ậ"]="a", ["ẩ"]="a", ["ẫ"]="a",
    ["ă"]="a", ["ằ"]="a", ["ắ"]="a", ["ặ"]="a", ["ẳ"]="a", ["ẵ"]="a",
    ["è"]="e", ["é"]="e", ["ẹ"]="e", ["ẻ"]="e", ["ẽ"]="e",
    ["ê"]="e", ["ề"]="e", ["ế"]="e", ["ệ"]="e", ["ể"]="e", ["ễ"]="e",
    ["ì"]="i", ["í"]="i", ["ị"]="i", ["ỉ"]="i", ["ĩ"]="i",
    ["ò"]="o", ["ó"]="o", ["ọ"]="o", ["ỏ"]="o", ["õ"]="o",
    ["ô"]="o", ["ồ"]="o", ["ố"]="o", ["ộ"]="o", ["ổ"]="o", ["ỗ"]="o",
    ["ơ"]="o", ["ờ"]="o", ["ớ"]="o", ["ợ"]="o", ["ở"]="o", ["ỡ"]="o",
    ["ù"]="u", ["ú"]="u", ["ụ"]="u", ["ủ"]="u", ["ũ"]="u",
    ["ư"]="u", ["ừ"]="u", ["ứ"]="u", ["ự"]="u", ["ử"]="u", ["ữ"]="u",
    ["ỳ"]="y", ["ý"]="y", ["ỵ"]="y", ["ỷ"]="y", ["ỹ"]="y",
    ["đ"]="d",

    ["À"]="A", ["Á"]="A", ["Ạ"]="A", ["Ả"]="A", ["Ã"]="A",
    ["Â"]="A", ["Ầ"]="A", ["Ấ"]="A", ["Ậ"]="A", ["Ẩ"]="A", ["Ẫ"]="A",
    ["Ă"]="A", ["Ằ"]="A", ["Ắ"]="A", ["Ặ"]="A", ["Ẳ"]="A", ["Ẵ"]="A",
    ["È"]="E", ["É"]="E", ["Ẹ"]="E", ["Ẻ"]="E", ["Ẽ"]="E",
    ["Ê"]="E", ["Ề"]="E", ["Ế"]="E", ["Ệ"]="E", ["Ể"]="E", ["Ễ"]="E",
    ["Ì"]="I", ["Í"]="I", ["Ị"]="I", ["Ỉ"]="I", ["Ĩ"]="I",
    ["Ò"]="O", ["Ó"]="O", ["Ọ"]="O", ["Ỏ"]="O", ["Õ"]="O",
    ["Ô"]="O", ["Ồ"]="O", ["Ố"]="O", ["Ộ"]="O", ["Ổ"]="O", ["Ỗ"]="O",
    ["Ơ"]="O", ["Ờ"]="O", ["Ớ"]="O", ["Ợ"]="O", ["Ở"]="O", ["Ỡ"]="O",
    ["Ù"]="U", ["Ú"]="U", ["Ụ"]="U", ["Ủ"]="U", ["Ũ"]="U",
    ["Ư"]="U", ["Ừ"]="U", ["Ứ"]="U", ["Ự"]="U", ["Ử"]="U", ["Ữ"]="U",
    ["Ỳ"]="Y", ["Ý"]="Y", ["Ỵ"]="Y", ["Ỷ"]="Y", ["Ỹ"]="Y",
    ["Đ"]="D",
}

function Led.ToAscii(text)

    if text == nil then
        return ""
    end

    text = tostring(text)

    for from, to in pairs(DIACRITICS) do
        text = text:gsub(from, to)
    end

    -- Anything still non-ASCII would be sent as raw high bytes and render as
    -- garbage, so drop it rather than display noise.
    text = text:gsub("[\128-\255]", "")

    -- The terminator is what ends a frame, so an embedded CR/LF would cut
    -- the message short mid-way. A run collapses to ONE space so "a\r\nb"
    -- reads as "a b" rather than "a  b".
    text = text:gsub("[\r\n]+", " ")

    return text

end

------------------------------------------------------------
-- Frame building
------------------------------------------------------------

-- Exposed (rather than local) so it can be unit-checked without a serial
-- port attached -- see led_test.lua.
function Led.BuildFrame(text)

    return string.char(CMD, ADDR, SNUM, SPEED, DIRECTION, COLOR) ..
           Led.ToAscii(text) ..
           string.char(TERMINATOR)

end

------------------------------------------------------------
-- Port lifecycle
------------------------------------------------------------

function Led.Open()

    if opened then
        return true
    end

    opened = Serial2.Open()

    return opened

end

function Led.Close()

    if opened then
        Serial2.Close()
        opened = false
    end

end

function Led.IsOpen()
    return opened
end

------------------------------------------------------------
-- Display
------------------------------------------------------------

function Led.Show(text)

    -- Opening lazily keeps every caller a one-liner: the workflow never has
    -- to care whether the display was reachable at startup.
    if not opened and not Led.Open() then
        return false
    end

    return Serial2.Write(Led.BuildFrame(text))

end

-- The TDM-800 in use is red-only (COLOR is fixed at 0x30), so severity can't
-- be shown as colour. These stay distinct entry points anyway: the workflow
-- reads better for it, and a future two-colour panel only has to change
-- here.
function Led.ShowSuccess(text)
    return Led.Show(text)
end

function Led.ShowError(text)
    return Led.Show(text)
end

function Led.ShowWarning(text)
    return Led.Show(text)
end

function Led.Clear()
    return Led.Show("")
end

return Led
