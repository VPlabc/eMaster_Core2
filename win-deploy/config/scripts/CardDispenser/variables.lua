-- variables.lua
-- Publishes runtime information for the monitoring Dashboard. Wraps the
-- native SetVariable/GetVariable bindings so the rest of the workflow deals
-- in named setters instead of raw variable-name strings.

local Variables = {}

function Variables.SetState(state)
    SetVariable("CurrentState", state)
end

function Variables.SetStep(step)
    SetVariable("CurrentStep", step or "")
end

function Variables.SetCitizen(citizenId, citizenName)
    SetVariable("CitizenID", citizenId or "")
    SetVariable("CitizenName", citizenName or "")
end

function Variables.SetCardUid(uid)
    SetVariable("CurrentCardUID", uid or "")
end

-- The card sitting inside the machine waiting for the next employee, or "" when
-- there isn't one. Distinct from CurrentCardUID, which is the card the run in
-- progress is working with.
function Variables.SetHeldCard(uid)
    SetVariable("HeldCardUID", uid or "")
end

-- Generic boolean, used for the machine status flags (CardNotInHopper,
-- CardRejectFull, CardSourceLow) so machine.lua can publish its issue list
-- without a named setter per sensor.
function Variables.SetFlag(name, value)
    SetVariable(name, value == true)
end

function Variables.SetRetryCount(count)
    SetVariable("RetryCount", count)
end

function Variables.SetLastError(message)
    SetVariable("LastError", message or "")
end

-- `response` is whatever Rest.PostForm() returned (a table); SetVariable
-- only accepts scalar values, so this stores a short human-readable summary.
function Variables.SetLastApiResponse(response)

    if response == nil then
        SetVariable("LastApiResponse", "no response")
        return
    end

    local summary = tostring(response.success)

    if response.message and response.message ~= "" then
        summary = summary .. ": " .. tostring(response.message)
    elseif response.status then
        summary = summary .. ": status " .. tostring(response.status)
    end

    SetVariable("LastApiResponse", summary)

end

function Variables.SetServerStatus(ok)
    SetVariable("ServerStatus", ok == true)
end

function Variables.SetRole(role)
    SetVariable("CitizenRole", role or "")
end

function Variables.SetCardMachine(machine)
    SetVariable("CardMachine", machine or "staff")
end

function Variables.UpdatePlcAndReaderStatus()
    SetVariable("PLCStatus", Modbus.IsConnected())
    SetVariable("ReaderStatus", Rfid.IsConnected())
end

return Variables
