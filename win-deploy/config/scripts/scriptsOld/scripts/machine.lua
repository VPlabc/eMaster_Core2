-- machine.lua
-- Consumable and jam status: no card in the hopper, reject bin full, card
-- stock low. Owns what to DO about those (block the workflow, raise a ticket
-- with the server, log it, tell the operator); the reads themselves belong to
-- plc.lua and the HTTP call to rest.lua.

local Config = require("config")
local Logger = require("logger")
local Plc = require("plc")
local RestApi = require("rest")
local Variables = require("variables")

local Machine = {}

------------------------------------------------------------
-- The three issues
------------------------------------------------------------

-- `blocking` means a card cannot be issued at all while the flag is set: an
-- empty hopper has nothing to dispense, so the workflow must not start and
-- must not move anything. A full reject bin and a low stock are reported and
-- displayed but still allow issuing -- the machine keeps working until the
-- consumable actually runs out.
local ISSUES = {

    {
        field   = "hopper_empty",
        code    = "card_not_in_hopper",
        address = Config.INPUT_CARD_NOT_IN_HOPPER,
        led     = "HOPPER_EMPTY",
        detail  = "No card in the hopper",
        variable = "CardNotInHopper",
        blocking = true,
    },

    {
        field   = "reject_full",
        code    = "card_reject_full",
        address = Config.INPUT_CARD_REJECT_FULL,
        led     = "REJECT_FULL",
        detail  = "Card reject bin is full",
        variable = "CardRejectFull",
        blocking = false,
    },

    {
        field   = "source_low",
        code    = "card_source_low",
        address = Config.INPUT_CARD_SOURCE_LOW,
        led     = "CARD_LOW",
        detail  = "Card source is low",
        variable = "CardSourceLow",
        blocking = false,
    },

}

------------------------------------------------------------
-- Reporting state
------------------------------------------------------------

-- Per issue: whether it was set the last time it was read, and when it was
-- last reported. Both drive the edge/repeat logic in report() below -- without
-- them, an idle machine standing with a full bin would post to the server
-- several times a second.
local reported = {}
local lastSent = {}

-- Cached last reading plus when it was taken, so callers can poll this module
-- from a 100ms loop without turning it into three Modbus reads per iteration.
local cached = nil
local cachedAt = 0

local function describe(issue)
    return issue.detail .. " (input " .. tostring(issue.address) .. ")"
end

-- Raises the issue with the server and writes it to the system error log. The
-- log line goes out first and unconditionally: the server is the part of this
-- that can be unreachable, and an issue that never reached it is exactly the
-- one worth having on disk.
local function send(issue, active)

    local detail = describe(issue)

    if active then
        Logger.LogError("MACHINE ISSUE: " .. detail)
    else
        Logger.LogInfo("Machine issue cleared: " .. detail)
    end

    local result = RestApi.ReportCardStatus(issue.code, active, detail, issue.address)

    -- Deliberately laxer than Utils.IsSuccess: this endpoint's response body is
    -- not specified yet, so a 2xx with no JSON (or with no `success` field)
    -- counts as delivered. Only a transport failure or an explicit
    -- success=false is treated as "the server did not take it".
    local delivered = result ~= nil and result.ok == true and result.success ~= false

    if not delivered then
        -- Not fatal and not retried here: the repeat timer below comes back to
        -- it, so a server that was down when the bin filled still learns about
        -- it within STATUS_REPEAT_SEC.
        Logger.LogError("Failed to report '" .. issue.code .. "' to the server: " ..
            tostring(result and (result.message or result.error or result.status) or "no response"))
        return false
    end

    Logger.LogInfo("Reported '" .. issue.code .. "' (" ..
        (active and "active" or "cleared") .. ") to the server")

    return true

end

-- Edge-triggered, with a repeat while the condition persists.
local function report(issue, active)

    local was = reported[issue.field] == true

    if active ~= was then

        reported[issue.field] = active
        lastSent[issue.field] = os.time()
        send(issue, active)

        return

    end

    if active and (os.time() - (lastSent[issue.field] or 0)) >= Config.STATUS_REPEAT_SEC then
        lastSent[issue.field] = os.time()
        send(issue, true)
    end

end

------------------------------------------------------------
-- Reading status
------------------------------------------------------------

-- Reads all three inputs, reports whatever changed, publishes the flags to the
-- Dashboard and returns:
--   { hopper_empty=, reject_full=, source_low=, blockKey= }
-- where blockKey is the messages.lua key of the blocking issue, or nil when a
-- card can be issued.
function Machine.Read()

    local raw = Plc.ReadMachineStatus()
    local status = { blockKey = nil }

    for _, issue in ipairs(ISSUES) do

        -- nil means the read failed. An unreachable PLC must not be mistaken
        -- for "hopper fine", but it must not raise a consumable ticket either
        -- -- the PLC connection is its own separate alarm (PLCStatus), so an
        -- unknown reading is left alone rather than reported as cleared.
        local value = raw[issue.field]

        status[issue.field] = value

        if value ~= nil then

            report(issue, value == true)

            if value == true and issue.blocking then
                status.blockKey = issue.led
            end

        end

        Variables.SetFlag(issue.variable, value == true)

    end

    cached = status
    cachedAt = os.time()

    return status

end

-- Same as Read(), but at most once every Config.STATUS_POLL_SEC. This is what
-- main.lua's idle loop calls.
function Machine.Poll()

    if cached ~= nil and (os.time() - cachedAt) < Config.STATUS_POLL_SEC then
        return cached
    end

    return Machine.Read()

end

-- Fresh read, for decisions that must not act on a two-second-old reading --
-- the workflow's go/no-go check before it starts moving cards.
function Machine.BlockKey()
    return Machine.Read().blockKey
end

-- messages.lua key -> the line that goes in the log / Last Error. Keeps the
-- wording in one place for both.
function Machine.Describe(ledKey)

    for _, issue in ipairs(ISSUES) do
        if issue.led == ledKey then
            return describe(issue)
        end
    end

    return "Machine not ready"

end

return Machine
