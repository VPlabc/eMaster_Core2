-- rest.lua
-- Wraps all HTTP communication with the Employee Registration Server. The
-- workflow never directly creates multipart/form-data -- it calls these
-- functions and gets back the { success, message, status } table that
-- Rest.PostForm() returns.

local Config = require("config")

Rest.SetServer(Config.SERVER_URL)
Rest.SetApiKey(Config.API_KEY)

local RestApi = {}

function RestApi.VerifyCitizen(citizen)

    local form = {

        citizen_id = citizen.citizen_id,
        citizen_name = citizen.full_name,
        birthday = citizen.birth,
        gender = citizen.gender,
        serial_number = citizen.serial_number,
        country = citizen.country,
        machine_id = Config.MACHINE_ID

    }

    return Rest.PostForm("/api/citizen/verify", form)

end

function RestApi.RegisterCard(citizenId, cardUid)

    local form = {

        citizen_id = citizenId,
        card_uid = cardUid,
        machine_id = Config.MACHINE_ID

    }

    return Rest.PostForm("/api/card/register", form)

end

function RestApi.CancelCard(citizenId, cardUid)

    local form = {

        employee_id = citizenId,
        card_uid = cardUid,
        machine_id = Config.MACHINE_ID

    }

    return Rest.PostForm("/api/card/cancel", form)

end

------------------------------------------------------------
-- Machine status / consumable issues
------------------------------------------------------------

-- Raises (or clears) a hardware issue with the server: reject bin full, card
-- stock low, nothing left in the hopper. `issue` is the machine-readable code,
-- `active` says whether the condition is present or has just cleared, and
-- `detail` is the human-readable line that also goes to the system error log.
--
-- The trailing slash is intentional -- the endpoint is "/api/card/status/", and
-- frameworks with APPEND_SLASH answer a slash-less POST with a redirect that
-- drops the form body.
function RestApi.ReportCardStatus(issue, active, detail, address)

    local form = {

        machine_id = Config.MACHINE_ID,
        issue = issue,
        status = active and "active" or "cleared",
        address = tostring(address or ""),
        message = detail or "",

    }

    return Rest.PostForm("/api/card/status/", form)

end

return RestApi
