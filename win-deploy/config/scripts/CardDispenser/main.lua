-- main.lua
-- Main workflow loop. Only controls the workflow and state transitions --
-- all hardware communication and business logic are encapsulated inside
-- the modules under require() below.

local Logger = require("logger")
local State = require("state")
local Variables = require("variables")
local Citizen = require("citizen")
local Workflow = require("workflow")
local Led = require("led")
local Plc = require("plc")
local Messages = require("messages")
local Machine = require("machine")

Logger.LogInfo("HSF Card Issuing Workflow started")

Variables.SetState(State.IDLE)
Variables.SetStep("Idle")
Variables.UpdatePlcAndReaderStatus()

Serial.Open()

-- Reset both dispenser blocks at startup. The workflow selects the block from
-- the role returned by /api/citizen/verify.
for _, machine in ipairs({ "staff", "contractor" }) do
    Plc.MoveCardIn(machine)
    Plc.CollectCard(machine)
end

if not Led.Open() then
    -- Non-fatal: the LED panel is operator guidance, not part of issuing a
    -- card. Every Led.Show() retries the open on its own, so the display
    -- starts working on its own once it's plugged in.
    Logger.LogWarning("LED display not available on Serial2; continuing without it")
end

-- What this loop believes is on the panel, so it writes to the LED only when the
-- message actually changes instead of ten times a second.
local prompt = nil

local function showPrompt(key)

    if key ~= prompt then
        prompt = key
        Led.Show(Messages.Get(key))
    end

end

showPrompt("WAITING_CITIZEN")

while true do

    Variables.UpdatePlcAndReaderStatus()

    -- Hopper / reject bin / card stock, on its own slower clock (Machine.Poll
    -- caches for Config.STATUS_POLL_SEC -- this loop spins every 100ms and a
    -- fresh check is three Modbus reads). Issues that appear or clear are posted
    -- to /api/card/status/ and written to the system error log in there.
    local status = Machine.Poll()

    -- A blocking issue takes over the panel: a machine with an empty hopper says
    -- so instead of inviting a scan it is going to refuse.
    showPrompt(status.blockKey or "WAITING_CITIZEN")

    local raw = Citizen.WaitCitizenCard()

    if raw ~= nil then

        Logger.LogInfo("Citizen Card Received")
        Workflow.IssueCardWorkflow(raw)
        Serial.Open()  -- WaitCitizenCard() closed it; reopen for the next card

        -- Back to Idle: reset the language, which the run may have switched to
        -- English for a non-VN citizen. The prompt is left to the next pass --
        -- the workflow wrote its own messages to the panel (and just held its
        -- closing one for ERROR_HOLD_MS), so `prompt` no longer describes what
        -- is displayed and has to be invalidated rather than guessed at.
        Messages.SetLanguage("VI")
        prompt = nil

    end

    Sleep(50)

end
