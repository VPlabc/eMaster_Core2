-- workflow.lua
-- Contains the complete card issuing business logic. Coordinates all other
-- modules; this is the only module that knows the full state machine.

local Config = require("config")
local Logger = require("logger")
local State = require("state")
local Variables = require("variables")
local Utils = require("utils")
local Citizen = require("citizen")
local CardClient = require("card")
local ZkCard = require("zkcard")
local Plc = require("plc")
local Led = require("led")
local Messages = require("messages")
local Machine = require("machine")

local Workflow = {}

-- The server reports "this employee already holds a valid card" only in its
-- message text, so it is matched as prose. Substring and case-insensitive, so
-- "Employee already has an active card" and "This employee already has an
-- active card." both hit.
local ACTIVE_CARD_PHRASE = "already has an active card"

-- A card the previous run could not hand over: the employee never took it out
-- of the slot, so the registration was cancelled and the card was pulled back
-- INSIDE the machine (Plc.MoveCardIn) instead of going to the collect bin. Its
-- code is kept here so the next employee who verifies OK is given that card --
-- nothing is dispensed, nothing is scanned, the UID is already known.
--
-- Module-level on purpose: main.lua calls IssueCardWorkflow() in a loop inside
-- one Lua state, so these survive between runs. They do NOT survive a script
-- restart, which is correct -- main.lua clears the transport at startup
-- (MoveCardIn + CollectCard), so a card held before a restart is in the bin and
-- must not be remembered as available.
local heldCardUid = nil
local heldCardUses = 0
local heldCardMachine = nil

local ROLE_FIELDS = { "role", "user_role", "staff_type", "employee_type", "type" }

-- Responses may be returned directly by the HTTP client or wrapped by an
-- endpoint helper (for example response.data, response.body.data, or
-- response.payload.data). Find the first non-empty role without assuming one
-- particular wrapper shape.
local function extractRole(response, visited)

    if type(response) == "string" then
        -- Some HTTP helpers expose the response body without decoding it.
        local role = response:match('"role"%s*:%s*"([^"]+)"')
        return role ~= nil and string.lower(role) or nil
    end
    if type(response) ~= "table" then
        return nil
    end

    visited = visited or {}
    if visited[response] then
        return nil
    end
    visited[response] = true

    for _, field in ipairs(ROLE_FIELDS) do
        local value = response[field]
        if value ~= nil and tostring(value) ~= "" then
            return string.lower(tostring(value))
        end
    end

    for _, field in ipairs({ "data", "response", "body", "payload", "result" }) do
        local value = extractRole(response[field], visited)
        if value ~= nil then
            return value
        end
    end

    -- Tolerate an additional wrapper name introduced by an HTTP client.
    for field, value in pairs(response) do
        if field ~= "data" and field ~= "response" and field ~= "body" and
           field ~= "payload" and field ~= "result" then
            local nested = extractRole(value, visited)
            if nested ~= nil then
                return nested
            end
        end
    end

    return nil

end

local function machineForRole(verifyResult)

    local rawRole = extractRole(verifyResult)

    if rawRole ~= nil then
        for roleText, machine in pairs(Config.ROLE_MACHINE) do
            if string.find(rawRole, string.lower(roleText), 1, true) ~= nil then
                return machine, rawRole
            end
        end
    end

    return "staff", rawRole or ""
end

local function setState(state, step)
    Variables.SetState(state)
    Variables.SetStep(step or "")
end

-- Every failure immediately updates Current State, Last Error, Dashboard
-- Variables and the System Log, then returns the workflow to Idle.
-- `ledKey` is the messages.lua key to show; omitted, the display is left
-- showing whatever it was (the caller already put a more specific message
-- there).
local function fail(message, ledKey)
    Logger.LogError(message)
    Variables.SetLastError(message)
    if ledKey ~= nil then
        Led.ShowError(Messages.Get(ledKey))
    end
    setState(State.ERROR, message)

    -- Keep the message readable before Idle: main.lua puts the idle prompt back
    -- as soon as this workflow returns, so a message that isn't held is a
    -- message nobody sees.
    Sleep(Config.ERROR_HOLD_MS)

    setState(State.IDLE, "")
end

------------------------------------------------------------
-- The held card
------------------------------------------------------------

-- Keeps the card inside the machine for the next employee instead of binning it.
-- Called AFTER the registration has been cancelled: the card must not be pulled
-- back in while it is still registered to someone who never received it.
local function holdCard(uid, machine)

    setState(State.HOLD_CARD, "User did not take card, holding card for the next employee")
    Plc.MoveCardIn(machine)

    heldCardUid = uid
    heldCardUses = heldCardUses + 1
    heldCardMachine = machine
    Variables.SetHeldCard(uid)

    Logger.LogInfo("Card " .. tostring(uid) .. " held inside the machine for the next employee" ..
        " (offered " .. heldCardUses .. "/" .. Config.MAX_CARD_REUSE .. " times)")

end

-- Bookkeeping only -- the caller decides what physically happens to the card
-- (binned with Plc.CollectCard, or carried off by the employee).
local function clearHeldCard()

    heldCardUid = nil
    heldCardUses = 0
    heldCardMachine = nil
    Variables.SetHeldCard(nil)

end

-- Puts a card in front of the registration step. Returns its UID (nil if none
-- turned up) and whether it came from the hold position rather than the hopper.
--
-- A held card is already inside the machine and its code was read on the run
-- that failed to hand it over, so there is nothing to dispense and nothing to
-- scan -- that is the whole point of holding it. Otherwise a fresh card is
-- dispensed to the scan position and read off the ZK controller (zkcard.lua);
-- waitResult.uid is the MSB hex the card is printed with (e.g. "709D52"), not
-- RTLog's raw decimal -- see ZkCard.CardHex.
local function acquireCard(machine)

    if heldCardUid ~= nil and heldCardMachine == machine then

        Logger.LogInfo("Reusing held card " .. heldCardUid .. " -- no card dispensed, no scan needed")
        Variables.SetCardUid(heldCardUid)

        return heldCardUid, true

    end

    setState(State.MOVE_CARD_TO_SCAN, "Moving card to scan position")

    setState(State.WAIT_CARD, "Waiting for card UID")
    Plc.MoveCardIn(machine)
    Plc.MoveCardToScan(machine)

    local waitResult = ZkCard.WaitCard(Config.CARD_WAIT_TIMEOUT)

    if not waitResult.ok then

        if waitResult.error ~= nil then
            Logger.LogWarning(waitResult.error)
        end

        return nil, false

    end

    Variables.SetCardUid(waitResult.uid)
    Logger.LogInfo("Card UID: " .. waitResult.uid ..
        " (raw " .. tostring(waitResult.raw) ..
        ", raw hex 0x" .. tostring(waitResult.raw_hex) ..
        ", LSB 0x" .. tostring(waitResult.uid_lsb) ..
        ", door " .. tostring(waitResult.door) ..
        " reader " .. tostring(waitResult.reader) .. ")")
    ZkCard.Clear()

    return waitResult.uid, false

end

function Workflow.IssueCardWorkflow(rawCitizenText)

    -- Hopper / reject bin / card stock. Machine.Read() also reports whatever
    -- appeared or cleared to the server and to the system error log, so that
    -- happens on every run as well as from main.lua's idle poll.
    --
    -- Only a BLOCKING issue stops the run -- no card in the hopper (input 1027).
    -- Starting anyway would move an empty carrier to the scan position, time out
    -- MAX_SCAN_RETRY times and finish by blaming the card reader. A held card is
    -- the exception: it is already inside the machine, so an empty hopper is
    -- irrelevant to handing it over.
    setState(State.VERIFY_CITIZEN, "Parsing citizen card")

    local citizen = Citizen.ParseCitizenCard(rawCitizenText)

    if citizen == nil then
        fail("Invalid Citizen Card", "CITIZEN_NOT_FOUND")
        return
    end

    -- Nationality decides the display language for the rest of this run,
    -- so select before the first citizen-specific message.
    Messages.SelectForCitizen(citizen)

    Variables.SetCitizen(citizen.citizen_id, citizen.full_name)
    Logger.LogInfo("Citizen ID : " .. citizen.citizen_id)
    Logger.LogInfo("Full Name  : " .. citizen.full_name)

    local verifyResult = Citizen.VerifyCitizen(citizen)
    Variables.SetLastApiResponse(verifyResult)
    Variables.SetServerStatus(verifyResult ~= nil)

    -- Employee already holds a valid card. Checked before the success test, not
    -- inside the failure branch: no second card may be issued either way, so it
    -- makes no difference whether the server calls this a rejection or answers
    -- success with the message attached. It is also not the same thing as "not
    -- registered" -- this person IS registered -- so it gets its own message
    -- rather than sending them off to register again.
    if Utils.MessageContains(verifyResult, ACTIVE_CARD_PHRASE) then
        fail(Utils.ResponseMessage(verifyResult, "Employee already has an active card"),
             "CARD_IS_ACTIVE")
        return
    end

    if not Utils.IsSuccess(verifyResult) then
        -- No response at all means the server is unreachable; a response
        -- that just says "not successful" means it answered and rejected
        -- the employee. Different messages for the operator.
        local ledKey = (verifyResult == nil) and "SERVER_ERROR" or "CITIZEN_NOT_FOUND"
        fail(Utils.ResponseMessage(verifyResult, "Citizen verification failed"), ledKey)
        return
    end

    local machine, role = machineForRole(verifyResult)

    -- A card held by block 1 must never be handed to a contractor (or vice
    -- versa). If the next role selects another block, collect the old held
    -- card and start with that block's hopper.
    if heldCardUid ~= nil and heldCardMachine ~= machine then
        Logger.LogWarning("Held card belongs to dispenser " .. tostring(heldCardMachine) ..
            "; collecting it before selecting " .. machine)
        Plc.CollectCard(heldCardMachine)
        clearHeldCard()
    end

    heldCardMachine = heldCardMachine or machine
    Variables.SetRole(role)
    Variables.SetCardMachine(machine)
    Logger.LogInfo("Citizen role: " .. (role ~= "" and role or "staff (default)") ..
        "; dispenser block " .. tostring(Config.MACHINES[machine].block))

    local blockKey = Machine.BlockKey(machine)
    if blockKey ~= nil and (heldCardUid == nil or heldCardMachine ~= machine) then
        fail(Machine.Describe(blockKey, machine), blockKey)
        return
    end

    Logger.LogInfo("Citizen Verify OK")
    Led.ShowSuccess(Messages.Get("CITIZEN_VERIFIED", citizen.full_name))

    local retryCount = 0
    Variables.SetRetryCount(retryCount)

    while true do

        -- A held card nobody claims after MAX_CARD_REUSE employees is treated as
        -- suspect: only its code is remembered, so a card taken out by hand (or
        -- a hold that silently failed) looks exactly like one still sitting
        -- inside. Bin it and fall through to a fresh one rather than offering a
        -- card that may not be there to yet another person.
        if heldCardUid ~= nil and heldCardUses >= Config.MAX_CARD_REUSE then

            Logger.LogWarning("Held card " .. heldCardUid .. " not taken by " ..
                Config.MAX_CARD_REUSE .. " employees; collecting it and dispensing a new one")
            setState(State.COLLECT_CARD, "Discarding held card")
            Plc.CollectCard(heldCardMachine)
            clearHeldCard()

        end

        -- Re-checked before every attempt that needs the hopper: verifying the
        -- citizen took a REST round trip, the hopper can run dry on the attempt
        -- that just failed, and the reject bin fills up as failed cards go into
        -- it. Skipped when a card is held -- that one needs no hopper.
        if heldCardUid == nil then

            local attemptBlock = Machine.BlockKey(machine)

            if attemptBlock ~= nil then
                fail(Machine.Describe(attemptBlock, machine), attemptBlock)
                return
            end

        end

        --Led.Show(Messages.Get("CARD_SCAN"))

        -- Either the card already held inside the machine (no dispense, no
        -- scan), or a fresh one dispensed to the scan position and read there.
        local cardUid, wasHeld = acquireCard(machine)

        if cardUid == nil then

            --------------------------------------------------------
            -- Card Timeout -> Collect Card -> Retry or Reader Error
            --
            -- Only reachable for a freshly dispensed card: a held card needs no
            -- scan, so acquireCard() never returns nil for one.
            --------------------------------------------------------

            Logger.LogWarning("Card timeout (retry " .. retryCount .. "/" .. Config.MAX_SCAN_RETRY .. ")")
            Led.ShowWarning(Messages.Get("NO_CARD"))
            setState(State.COLLECT_CARD, "Card timeout, collecting card")
            Plc.CollectCard(machine)
            Sleep(500)
            --Led.Show(Messages.Get("CARD_COLLECTED"))

            if retryCount >= Config.MAX_SCAN_RETRY then
                fail("Card reader error: no card received after " .. Config.MAX_SCAN_RETRY .. " retries",
                     "READER_ERROR")
                return
            end

            retryCount = retryCount + 1
            Variables.SetRetryCount(retryCount)
            -- loop back to Move Card To Scan

        else

            setState(State.REGISTER_CARD, wasHeld and "Registering held card" or "Registering card")
            local regResult = CardClient.RegisterCard(citizen.citizen_id, cardUid)
            Variables.SetLastApiResponse(regResult)
            Variables.SetServerStatus(regResult ~= nil)

            if not Utils.IsSuccess(regResult) then

                ----------------------------------------------------
                -- Register Fail -> Collect Card -> Retry or Reader Error
                ----------------------------------------------------

                Logger.LogWarning("Register failed (retry " .. retryCount .. "/" .. Config.MAX_SCAN_RETRY .. "): " ..
                    Utils.ResponseMessage(regResult, "unknown error"))
                Led.ShowError(Messages.Get(regResult == nil and "SERVER_ERROR" or "CITIZEN_NOT_FOUND"))
                setState(State.COLLECT_CARD, "Register failed, collecting card")
                Plc.CollectCard(machine)
                Sleep(500)
                --Led.Show(Messages.Get("CARD_COLLECTED"))

                -- The collect above binned the card, held or not, so stop
                -- remembering it -- a retry has to dispense a fresh one because
                -- the transport is empty now.
                if wasHeld then
                    Logger.LogWarning("Held card " .. cardUid .. " discarded after a failed registration")
                end

                clearHeldCard()

                -- Employee already holds a valid card. Matched on the response
                -- TEXT: regResult is the { success, message, status } table, so
                -- comparing it against a string never matched. No retry --
                -- another attempt gets the same answer.
                if Utils.MessageContains(regResult, ACTIVE_CARD_PHRASE) then
                    fail(Utils.ResponseMessage(regResult, "Employee already has an active card"),
                         "CARD_IS_ACTIVE")
                    return
                end

                if retryCount >= Config.MAX_SCAN_RETRY then
                    fail("Card registration failed after " .. Config.MAX_SCAN_RETRY .. " retries",
                         "SERVER_ERROR")
                    return
                end

                retryCount = retryCount + 1
                Variables.SetRetryCount(retryCount)
                -- loop back to Move Card To Scan

            else

                ----------------------------------------------------
                -- Register OK -> Move Card Out -> Wait User Take Card
                ----------------------------------------------------

                Logger.LogInfo("Card Registered OK for employee " .. citizen.citizen_id)
                --Led.ShowSuccess(Messages.Get("CARD_REGISTERED"))

                setState(State.MOVE_CARD_OUT, "Presenting card to user")

                setState(State.WAIT_USER_TAKE, "Waiting for user to take card")
                Plc.MoveCardIn(machine)
                Plc.MoveCardOut(machine)
                -- Tell the user to take the card as soon as it is presented.
                -- WaitCardTaken() blocks for up to TAKE_CARD_TIMEOUT, so sending
                -- this after it would make the LED update arrive only once the
                -- card was already taken (or after the timeout).
                Led.Show(Messages.Get("TAKE_CARD"))
                local taken = Plc.WaitCardTaken(Config.TAKE_CARD_TIMEOUT, machine)

                if taken then

                    -- The card left the machine, so nothing is held any more (a
                    -- no-op unless this run handed over a held card).
                    clearHeldCard()

                    setState(State.FINISH, "Card issued successfully")
                    Logger.LogInfo("Card issued successfully")
                    Led.ShowSuccess(Messages.Get("THANK_YOU"))
                    setState(State.IDLE, "")
                    Sleep(3000)

                else

                    ------------------------------------------------
                    -- Not Taken -> Cancel Registration -> HOLD the
                    -- card for the next employee
                    ------------------------------------------------

                    Logger.LogWarning("User did not take the card in time, cancelling registration")

                    --Led.ShowWarning(Messages.Get("CARD_TIMEOUT"))
                    -- Cancel FIRST, while the code still belongs to this
                    -- employee. The card is about to be handed to whoever
                    -- verifies next, and it must not spend that time still
                    -- registered to someone who never received it.
                    local cancelResult = CardClient.CancelCard(citizen.citizen_id, cardUid)
                    Variables.SetLastApiResponse(cancelResult)

                    -- Then keep the card instead of binning it: pulled back
                    -- inside, code saved, offered to the next employee who
                    -- verifies OK. That run skips the dispense and the scan and
                    -- goes straight to registering this code; if the register
                    -- succeeds it continues through the normal Move Card Out /
                    -- Wait User Take path.

                    holdCard(cardUid, machine)
                    fail("User did not take card, registration cancelled; card held for the next employee","CANCELLED")
                        
                   

                end

                return

            end

        end

    end

end

return Workflow
