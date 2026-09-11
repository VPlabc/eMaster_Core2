-- card.lua
-- Card Reader Client communication. No PLC logic in this module.

local RestApi = require("rest")

local CardClient = {}

------------------------------------------------------------
-- Wait for a card UID via the Card Reader Client API cache
------------------------------------------------------------

function CardClient.WaitCard(timeoutMs)

    local elapsed = 0

    while elapsed < timeoutMs do

        if Card.Available() then
            local entry = Card.Get()
            return { ok = true, uid = entry.uid, position = entry.position }
        end

        Sleep(1)
        elapsed = elapsed + 1000

    end

    return { ok = false }

end

------------------------------------------------------------
-- Register / Cancel (delegate the actual HTTP call to rest.lua)
------------------------------------------------------------

function CardClient.RegisterCard(citizenId, cardUid)
    return RestApi.RegisterCard(citizenId, cardUid)
end

function CardClient.CancelCard(citizenId, cardUid)
    return RestApi.CancelCard(citizenId, cardUid)
end

------------------------------------------------------------
-- Clear cache
------------------------------------------------------------

function CardClient.ClearCard()
    Card.Clear()
end

return CardClient
