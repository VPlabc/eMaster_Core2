-- led_test.lua
-- Checks led.lua's frame encoding against the worked example in
-- request/LEDBoard.md, plus messages.lua's language selection. Pure
-- byte-building -- no LED panel or Serial2 port has to be present, so this
-- is safe to run anywhere.
--
-- Run as a FILE (Lua Editor / POST /api/lua/scripts/led_test.lua/run) so
-- require() can resolve the sibling modules.

local Led = require("led")
local Messages = require("messages")

local failed = false

local function Check(ok, message)
    if ok then
        Log.Info("led_test PASS: " .. message)
    else
        Log.Error("led_test FAIL: " .. message)
        failed = true
    end
end

local function ToHex(s)
    return (s:gsub(".", function(c) return string.format("%02X ", c:byte()) end))
end

------------------------------------------------------------
-- Frame layout, against the spec's own example
------------------------------------------------------------

-- request/LEDBoard.md "Example Frame": header 00 30 31 31 32 30, then
-- "Please Take Card" in ASCII, then 0D.
local frame = Led.BuildFrame("Please Take Card")

local expected = string.char(0x00, 0x30, 0x31, 0x31, 0x30, 0x30) ..
                 "Please Take Card" ..
                 string.char(0x0D)

Check(frame == expected, "frame matches LEDBoard.md example -> " .. ToHex(frame))

Check(frame:byte(1) == 0x00, "CMD = 0x00")
Check(frame:byte(2) == 0x30, "ADDR = 0x30 (broadcast)")
Check(frame:byte(3) == 0x31, "SNUM = 0x31 (one scene)")
Check(frame:byte(4) == 0x31, "SPEED = 0x31")
Check(frame:byte(5) == 0x30, "DIRECTION = 0x30 (static)")
Check(frame:byte(6) == 0x30, "COLOR = 0x30 (red)")
Check(frame:byte(#frame) == 0x0D, "terminator = 0x0D (without it the panel never updates)")

------------------------------------------------------------
-- ASCII conversion
------------------------------------------------------------

Check(Led.ToAscii("Moi quet CCCD") == "Moi quet CCCD", "plain ASCII passes through unchanged")

-- The panel is ASCII-only, so a name straight off a citizen card has to be
-- flattened rather than sent as raw UTF-8 high bytes.
Check(Led.ToAscii("NGUYỄN VĂN A") == "NGUYEN VAN A",
    "diacritics stripped: NGUYỄN VĂN A -> " .. Led.ToAscii("NGUYỄN VĂN A"))
Check(Led.ToAscii("Xin chào") == "Xin chao", "lowercase diacritics stripped")

-- An embedded CR would end the frame early and truncate the message.
Check(Led.ToAscii("a\r\nb") == "a b", "CR/LF replaced with space, not left to end the frame")

Check(Led.ToAscii(nil) == "", "nil text -> empty string (no crash)")

local clearFrame = Led.BuildFrame("")
Check(#clearFrame == 7, "empty text still sends a valid 7-byte frame (header + terminator)")

------------------------------------------------------------
-- Messages / language selection
------------------------------------------------------------

Messages.SelectForCitizen({ country = "VNM" })
Check(Messages.Get("WAITING_CITIZEN") == "Moi quet CCCD", "VNM citizen -> Vietnamese")
Check(Messages.Get("CITIZEN_VERIFIED", "NGUYEN VAN A") == "Xin chao NGUYEN VAN A",
    "name substituted into Vietnamese greeting")

Messages.SelectForCitizen({ country = "USA" })
Check(Messages.Get("WAITING_CITIZEN") == "Please Scan Citizen Card", "non-VNM citizen -> English")
Check(Messages.Get("CITIZEN_VERIFIED", "JOHN SMITH") == "Welcome JOHN SMITH",
    "name substituted into English greeting")

Messages.SelectForCitizen(nil)
Check(Messages.Get("WAITING_CITIZEN") == "Moi quet CCCD", "nil citizen falls back to Vietnamese")

Check(Messages.Get("NO_SUCH_KEY") == "NO_SUCH_KEY", "unknown key returns the key, doesn't blank the display")

Messages.SetLanguage("VI")

Log.Info(failed and "led_test: FAIL" or "led_test: PASS")
