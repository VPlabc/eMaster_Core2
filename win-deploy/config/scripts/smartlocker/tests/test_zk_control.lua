-- tests/test_zk_control.lua
-- Relays, auxiliary outputs and inputs on the ZK access controller.
--
-- This is the test for the half of the panel the card reader does not cover:
-- ControlDevice (sdk-protocol-reference.md section 3.5) and what the RTLog
-- reports about the panel's own I/O.
--
-- WHAT THE PROTOCOL DOES AND DOES NOT OFFER, since both shape this test:
--
--   * Outputs are WRITE ONLY. ControlDevice closes a door relay or an auxiliary
--     output; there is no call that asks what an output is doing. Every check
--     below is therefore "the command was accepted", and the operator confirms
--     the rest by watching the relay LED.
--   * Inputs are EVENT ONLY. There is no read-input call either: an auxiliary
--     input announces itself as RTLog event 220 (disconnected) or 221
--     (shorted), and door sensors arrive as a door/alarm status record. So an
--     input that nobody has touched since the gateway started has no state, and
--     zk.inputState() answers nil rather than guessing.
--   * There is NO beeper command anywhere in the SDK. zk.beep() pulses an
--     output; what it sounds like depends on what is wired to it.
--
-- THIS TEST OPERATES RELAYS. Door 1 is pulsed. Run it with the door harmless.

local ok = pcall(require, "bootstrap")
if not ok then require("smartlocker.tests.bootstrap") end

local Config  = require("config")
local Harness = require("tests.harness")
local Reader  = require("hardware.zk_reader")

local zk = nil
do
    local loaded, module = pcall(require, "zk_controller")
    zk = loaded and module or nil
end

-- Set to nil to skip the door-relay part on a panel whose door 1 is a real
-- door somebody walks through.
local TEST_DOOR = 1
local TEST_AUX = Config.buzzer.aux_output or 1
local WATCH_INPUTS_SECONDS = 30

Harness.start("ZK controller -- relays, aux outputs and inputs")

if zk == nil then
    Harness.skip("everything", "the zk_controller module is not available in this build")
    Harness.finish()
    return
end

if type(zk.controlDevice) ~= "function" then
    Harness.skip("everything", "this gateway build predates the ControlDevice bindings")
    Harness.finish()
    return
end

------------------------------------------------------------
-- Connect
------------------------------------------------------------

Harness.section("Connection")

local connected, connectError = Reader.connect()

if not Harness.check("controller connected", connected, connectError) then
    Harness.finish()
    return
end

------------------------------------------------------------
-- What this panel has
------------------------------------------------------------

Harness.section("Panel capabilities")

local params, paramError = zk.getParam("LockCount,AuxOutCount,AuxInCount,ReaderCount,~SerialNumber")

if params == nil then
    Harness.check("GetDeviceParam answered", false, paramError)
else
    Harness.check("GetDeviceParam answered", true)
    Harness.info("Serial number : " .. tostring(params["~SerialNumber"]))
    Harness.info("Door relays   : " .. tostring(params.LockCount))
    Harness.info("Aux outputs   : " .. tostring(params.AuxOutCount))
    Harness.info("Aux inputs    : " .. tostring(params.AuxInCount))
    Harness.info("Readers       : " .. tostring(params.ReaderCount))

    local auxOut = tonumber(params.AuxOutCount) or 0
    Harness.check("the configured buzzer output exists on this panel", TEST_AUX <= auxOut,
                  "buzzer.aux_output = " .. tostring(TEST_AUX) .. ", panel has " .. auxOut)
end

-- The same numbers as the driver cached them at connect time.
local io = zk.ioState()

Harness.check("ioState() reports the panel's counts",
              io.counts ~= nil and (io.counts.locks or 0) > 0,
              io.counts and ("locks=" .. tostring(io.counts.locks) ..
                              " aux_out=" .. tostring(io.counts.aux_out) ..
                              " aux_in=" .. tostring(io.counts.aux_in)) or "no counts")

------------------------------------------------------------
-- Auxiliary output
------------------------------------------------------------

Harness.section("Auxiliary output " .. TEST_AUX)

Harness.info("Watch the AUX" .. TEST_AUX .. " relay LED on the panel.")

Harness.check("one second hold accepted", zk.auxOut(TEST_AUX, 1) == true, zk.lastError())
Sleep(1500)

-- Millisecond control: latch (255) then release (0). The whole reason the
-- driver has PulseOutput at all -- ControlDevice's own duration is in seconds.
Harness.check("150 ms pulse accepted", zk.pulse(TEST_AUX, 150, true) == true, zk.lastError())
Sleep(500)

Harness.check("three beeps accepted", zk.beep(TEST_AUX, 3, 120, 120, true) == true, zk.lastError())

------------------------------------------------------------
-- Door relay
------------------------------------------------------------

if TEST_DOOR ~= nil then

    Harness.section("Door relay " .. TEST_DOOR)

    Harness.info("The door relay will close for 3 seconds.")
    Harness.check("openDoor accepted", zk.openDoor(TEST_DOOR, 3) == true, zk.lastError())
    Sleep(3500)

    -- Releasing explicitly: 0 means "off now", whatever hold was requested.
    Harness.check("explicit release accepted", zk.controlDevice(1, TEST_DOOR, 1, 0, 0) == true, zk.lastError())

else
    Harness.skip("door relay", "TEST_DOOR is nil in this script")
end

------------------------------------------------------------
-- Door sensors
------------------------------------------------------------

Harness.section("Door sensors")

-- Status records only arrive when the panel has no events queued, so this may
-- legitimately be empty on a busy controller.
local state = zk.ioState()

if not state.status_seen then
    Harness.skip("door status", "the panel has not sent a status record yet -- leave it idle and re-run")
else
    Harness.check("door status received", true, "at " .. tostring(state.status_time))
    for index, door in ipairs(state.doors or {}) do
        Harness.info(string.format("   door %d: %s", index, tostring(door)))
    end
    Harness.info("NO_SENSOR means Door<n>SensorType is 0 on the panel, not that the door is missing.")
end

------------------------------------------------------------
-- Auxiliary inputs
------------------------------------------------------------

Harness.section("Auxiliary inputs -- " .. WATCH_INPUTS_SECONDS .. "s")

Harness.info("Trigger an input (a button, a sensor). Events 220/221 should appear.")

local seen = {}

zk.onAuxInput(function(input, shorted)
    seen[#seen + 1] = { input = input, shorted = shorted }
    Harness.info("   input " .. tostring(input) .. " -> " .. (shorted and "SHORTED" or "OPEN"))
end)

local elapsed = 0
while elapsed < WATCH_INPUTS_SECONDS * 1000 do
    Sleep(100)   -- also what delivers the queued callbacks
    elapsed = elapsed + 100
end

Harness.check("an input edge was seen", #seen > 0,
              #seen .. " edge(s) -- zero means nothing was triggered, or nothing is wired")

if #seen > 0 then
    local first = seen[1].input
    Harness.check("inputState() remembers the last edge", zk.inputState(first) ~= nil)
end

Harness.check("an untouched input reports nil rather than false", zk.inputState(99) == nil)

Harness.info("")
Harness.info("Checklist:")
Harness.info("  [ ] the aux relay clicked for each pulse")
Harness.info("  [ ] the buzzer (if wired to it) sounded three times")
Harness.info("  [ ] the door relay closed and released")
Harness.info("  [ ] each wired input reported its own number")

Harness.finish()
