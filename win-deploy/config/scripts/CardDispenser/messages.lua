-- messages.lua
-- LED display text, per request/LEDBoard.md's two message tables. Kept out
-- of led.lua (which owns the wire protocol) and out of workflow.lua (which
-- owns the state machine) so wording can change without touching either.
--
-- Vietnamese is written WITHOUT diacritics on purpose -- the TDM-800 is
-- ASCII-only. Led.ToAscii() would strip them anyway, but writing them out
-- plainly here keeps what's in this file identical to what appears on the
-- panel.

local Messages = {}

Messages.VI = {
    WAITING_CITIZEN   = "Moi quet CCCD",
    CITIZEN_VERIFIED  = "Xin chao %s",
    CITIZEN_NOT_FOUND = "Nhan vien chua dang ky",
    CARD_IS_ACTIVE    = "Nhan vien da duoc cap the",
    CARD_SCAN         = "Dang cap the...",
    NO_CARD           = "Khong tim thay the",
    CARD_REGISTERED   = "Dang ky the thanh cong",
    TAKE_CARD         = "Vui long nhan the",
    CARD_TIMEOUT      = "Het thoi gian nhan the",
    CARD_COLLECTED    = "The da duoc thu hoi",
    CANCELLED         = "Vui long thu lai",--Huy dang ky
    HOPPER_EMPTY      = "May het the - Lien he ky thuat",
    REJECT_FULL       = "Hop the day - Lien he ky thuat",
    CARD_LOW          = "The trong may sap het",
    READER_ERROR      = "Loi dau doc the",
    PLC_ERROR         = "Loi ket noi PLC",
    SERVER_ERROR      = "Loi ket noi Server",
    THANK_YOU         = "Cam on",
}

Messages.EN = {
    WAITING_CITIZEN   = "Please Scan Citizen Card",
    CITIZEN_VERIFIED  = "Welcome %s",
    CITIZEN_NOT_FOUND = "Employee Not Found",
    CARD_IS_ACTIVE    = "Employee is active",
    CARD_SCAN         = "Issuing Card...",
    NO_CARD           = "Card Not Detected",
    CARD_REGISTERED   = "Card Registered Successfully",
    TAKE_CARD         = "Please Take Your Card",
    CARD_TIMEOUT      = "Card Pickup Timeout",
    CARD_COLLECTED    = "Card Returned",
    CANCELLED         = "Try again",--Registration Cancelled
    HOPPER_EMPTY      = "Out Of Cards - Contact Support",
    REJECT_FULL       = "Reject Bin Full - Contact Support",
    CARD_LOW          = "Card Stock Low",
    READER_ERROR      = "Card Reader Error",
    PLC_ERROR         = "PLC Communication Error",
    SERVER_ERROR      = "Server Connection Error",
    THANK_YOU         = "Thank You",
}

-- Vietnamese until a citizen is identified as something else -- the machine
-- sits in Vietnam, and the idle prompt has to be in *some* language before
-- any card has been read.
local current = Messages.VI

-- citizen.country is "VNM" for every card citizen.lua accepts today (it only
-- parses IDVNM documents), so in practice this stays Vietnamese. Written
-- generically anyway so a future non-VN document type switches languages on
-- its own, per request/LEDBoard.md's "Language Selection".
function Messages.SelectForCitizen(citizen)

    if citizen ~= nil and citizen.country ~= nil and citizen.country ~= "VNM" then
        current = Messages.EN
    else
        current = Messages.VI
    end

    return current

end

function Messages.SetLanguage(lang)
    current = (lang == "EN") and Messages.EN or Messages.VI
end

-- Get("CITIZEN_VERIFIED", name) -> "Xin chao NGUYEN VAN A"
function Messages.Get(key, arg)

    local text = current[key]

    if text == nil then
        return key   -- unknown key: show it rather than blanking the display
    end

    if arg ~= nil and text:find("%%s") then
        return string.format(text, tostring(arg))
    end

    return text

end

return Messages
