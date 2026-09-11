-- tests/test_buzzer.lua
-- Stage 1.5 (Plan section 22): the buzzer patterns.
--
-- Read hardware/buzzer.lua's header first: nothing in the PullSDK addresses the
-- reader's own sounder, so the buzzer is whatever relay it is wired to -- an
-- auxiliary output on the access controller (backend "zk") or a PLC coil
-- (backend "plc"). With buzzer.enabled false every call below succeeds
-- silently -- which is the correct behaviour, and why this test reports SKIP
-- rather than FAIL in that case.

local ok = pcall(require, "bootstrap")
if not ok then require("smartlocker.tests.bootstrap") end

local Buzzer  = require("hardware.buzzer")
local Config  = require("config")
local Harness = require("tests.harness")
local Modbus  = require("hardware.modbus")

Harness.start("Stage 1.5 -- buzzer")

local status = Buzzer.status()

Harness.info("Enabled: " .. tostring(status.enabled))
Harness.info("Backend: " .. tostring(status.backend))

if status.backend == "zk" then
    Harness.info("Output:  ZK " .. (status.relay_kind == "aux" and "auxiliary output" or "user relay") ..
                 " " .. tostring(status.relay) ..
                 (status.zk_available and "" or "  (zk_controller module NOT available)"))
else
    Harness.info("Output:  register " .. tostring(status.register) .. " bit " .. tostring(status.bit))
end

if not status.usable then

    Harness.skip("buzzer patterns", "buzzer.enabled is false, or no output is configured")

    Harness.info("")
    Harness.info("To enable, in smartlocker.json:")
    Harness.info('   "buzzer": { "enabled": true, "backend": "zk",  "relay": 1, "relay_kind": "user" }')
    Harness.info('   (relay_kind is "user" for a lock relay or "aux" for an auxiliary output --')
    Harness.info('    find the real number on the Test Tools page, Relay & Beeper)')
    Harness.info("or")
    Harness.info('   "buzzer": { "enabled": true, "backend": "plc", "register": 40001, "bit": 15 }')
    Harness.info("Every call still succeeds while it is off, so access decisions are unaffected.")

    -- The calls are still exercised, to prove the disabled path is harmless.
    Harness.check("beep(1) with the buzzer off", Buzzer.beep(1) == true)
    Harness.check("wrong_locker() with the buzzer off", Buzzer.wrong_locker() == true)

    Harness.finish()
    return

end

-- Only the PLC backend needs the PLC; the ZK backend needs the controller,
-- which hardware/buzzer.lua reports on through usable() above.
if status.backend == "plc" then

    local connected, connectError = Modbus.connect()

    if not Harness.check("PLC is reachable", connected, connectError) then
        Harness.finish()
        return
    end

end

------------------------------------------------------------
-- Patterns
------------------------------------------------------------

local function pattern(name, description, fn)

    Harness.info("")
    Harness.info(description)

    local started = os.time()
    local result = fn()
    local took = os.time() - started

    Harness.check(name, result == true, "took about " .. took .. "s")

    Sleep(1000)

end

Harness.section("Patterns")

pattern("one beep", "Expect: ONE short beep", function()
    return Buzzer.beep(1)
end)

pattern("five beeps", "Expect: FIVE short beeps (the wrong-locker signal, Plan section 9)", function()
    return Buzzer.wrong_locker()
end)

pattern("long beep", "Expect: ONE long beep (the expired-card signal, Plan section 10)", function()
    return Buzzer.expired_card()
end)

pattern("granted", "Expect: the access-granted signal", function()
    return Buzzer.granted()
end)

pattern("denied", "Expect: the access-denied signal", function()
    return Buzzer.denied()
end)

pattern("door error", "Expect: the door-timeout signal (Plan section 11)", function()
    return Buzzer.door_error()
end)

------------------------------------------------------------
-- The output must not be left energised
------------------------------------------------------------

Harness.section("Output state")

if status.backend == "plc" then

    Modbus.begin_scan()

    local held = Modbus.read_register_bit(Config.buzzer.register, Config.buzzer.bit)

    Harness.check("buzzer output is off afterwards", held == false,
                  held == nil and "the register could not be read" or nil)

else

    -- The controller has no read-output call (PullSDK exposes ControlDevice
    -- only, one way), so this one has to be verified by ear and by looking at
    -- the relay LED. A buzzer still sounding here means the release half of the
    -- pulse failed -- which the driver logs as an error.
    Harness.skip("buzzer output is off afterwards",
                 "the ZK panel cannot be asked the state of an output; listen instead")

end

Harness.info("")
Harness.info("Checklist for Plan section 22:")
Harness.info("  [ ] each pattern is distinguishable from the others by ear")
Harness.info("  [ ] five beeps take under two seconds (they hold up the door)")
Harness.info("  [ ] the buzzer is silent when idle")

Harness.finish()
