-- citizen.lua
-- Citizen ID card parser & verification. No PLC control should exist in
-- this module.

local RestApi = require("rest")

local Citizen = {}

------------------------------------------------------------
-- Parse Citizen ID Card
------------------------------------------------------------

function Citizen.ParseCitizenCard(raw)

    if raw == nil then
        return nil
    end

    if string.sub(raw, 1, 5) ~= "IDVNM" then
        return nil
    end

    local card = {}

    --------------------------------------------------------
    -- Fixed Length Fields
    --------------------------------------------------------

    card.raw = raw

    card.document_code =
        string.sub(raw, 1, 5)

    card.serial_number =
        string.sub(raw, 6, 15)

    card.citizen_id =
        string.sub(raw, 16, 27)

    card.birth =
        string.sub(raw, 30, 35)

    card.gender =
        string.sub(raw, 38, 38)

    card.country = "VNM"

    --------------------------------------------------------
    -- Name
    --------------------------------------------------------

    local p = string.find(raw, "VNM<<<<<<<<<<<")

    if p ~= nil then

        local names =
            string.sub(raw, p + 15)

        names =
            string.gsub(names, "<+$", "")

        local t = {}

        for s in string.gmatch(names, "([^<]+)") do
            table.insert(t, s)
        end

        if #t >= 3 then

            card.last_name = t[1]
            card.middle_name = t[2]
            card.first_name = t[3]

        elseif #t == 2 then

            card.last_name = t[1]
            card.first_name = t[2]
            card.middle_name = ""

        elseif #t == 1 then

            card.first_name = t[1]

        end

        card.full_name =
            (card.last_name or "") ..
            " " ..
            (card.middle_name or "") ..
            " " ..
            (card.first_name or "")

    end

    return card

end

------------------------------------------------------------
-- Verify Citizen (delegates the actual HTTP call to rest.lua)
------------------------------------------------------------

function Citizen.VerifyCitizen(card)
    return RestApi.VerifyCitizen(card)
end

------------------------------------------------------------
-- Wait Citizen Card (USB Serial)
------------------------------------------------------------

-- Non-blocking single check: returns the raw card text and closes the
-- serial port if a line arrived, or nil if nothing has been read yet. The
-- caller (main.lua) owns the poll loop and reopens the port before calling
-- this again.
function Citizen.WaitCitizenCard()

    local raw = Serial.Read()

    if raw ~= nil then
        --Serial.Close()
        return raw
    end

    return nil

end

return Citizen
