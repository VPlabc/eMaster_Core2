-- hardware/buzzer.lua
-- Audible feedback (Plan section 22).
--
-- WHERE THE BUZZER IS. Not on the reader itself: nothing in the ZKTeco PullSDK
-- addresses the reader's own sounder -- plcommpro.dll exposes Connect,
-- ControlDevice, Get/SetDeviceParam, Get/SetDeviceData, GetRTLog and file
-- transfer, and the parameter table (sdk-protocol-reference.md, Attached Table
-- 2) has no beeper entry either. An audible signal is therefore whatever RELAY
-- the buzzer is wired to, and there are two of those:
--
--   backend = "zk"    a relay on the access controller, pulsed through
--                     ControlDevice operation 1. `relay_kind` picks which
--                     address type that is:
--                       "user" -> a lock/door relay   (address type 1)
--                       "aux"  -> an auxiliary output (address type 2)
--                     Sub-second beeps work because the driver latches the
--                     output and releases it, rather than using the SDK's
--                     whole-second hold.
--   backend = "plc"   a Modbus coil or register bit on the PLC, pulsed exactly
--                     like an unlock output.
--
-- WHICH RELAY IS IT? That is a wiring question, not a software one, and it is
-- what the gateway's Test Tool (Test Tools page -> Relay & Beeper) exists to
-- answer: step through the relays until you hear it, then put that number and
-- kind here. Guessing costs a beep nobody hears; guessing a LOCK relay wrong
-- opens a door.
--
-- Whichever is chosen, it stays DISABLED until smartlocker.json names the
-- output. With it off every call still succeeds and still logs -- a denied card
-- is refused and recorded whether or not anyone can hear it. That is
-- deliberate: silence must never be the reason a locker opens.
--
-- Each pattern costs real time (five beeps at 150 ms on and off is 1.5
-- seconds), and that time is spent inside a blocking call, so nothing else in
-- the script runs during it. Keep the patterns short.

local Config = require("config")
local Logger = require("utils.logger")
local Modbus = require("hardware.modbus")

local zk = nil

do
    -- Same soft dependency as hardware/zk_reader.lua: the module is registered
    -- by the gateway, but every call fails on a build without the PullSDK.
    local ok, module = pcall(require, "zk_controller")
    zk = ok and module or nil
end

local Buzzer = {}

local settings = Config.buzzer

-- Which ZK relay carries the sounder, as a (number, auxiliary) pair.
--
-- `relay` is the canonical setting. `aux_output` is still honoured because it
-- is what every installed smartlocker.json says today, and a rename that
-- silently un-configured a working buzzer would be a poor trade for a tidier
-- name -- but a file that sets it, and nothing else, keeps the OLD auxiliary
-- meaning rather than being quietly moved onto a lock relay.
local function zkRelay()

    if settings.relay ~= nil then
        return settings.relay, (settings.relay_kind or "user") == "aux"
    end

    if settings.aux_output ~= nil then
        return settings.aux_output, true
    end

    return nil, false

end

-- Misconfiguration is reported once, not on every beep: the buzzer is called
-- from every access decision, and a warning per swipe would bury the swipes.
local complained = false

local function complain(message)

    if not complained then
        complained = true
        Logger.warning(message)
    end

end

local function usable()

    if not settings.enabled then
        return false
    end

    if settings.backend == "zk" then

        if zk == nil then
            complain("buzzer.backend is \"zk\" but the zk_controller module is not available in this build")
            return false
        end

        if zkRelay() == nil then
            complain("buzzer.backend is \"zk\" but neither buzzer.relay nor buzzer.aux_output " ..
                     "is configured -- find the relay with the Test Tools page, Relay & Beeper")
            return false
        end

        return true

    end

    if settings.backend ~= "plc" then
        complain("buzzer backend '" .. tostring(settings.backend) .. "' is not implemented; buzzer disabled")
        return false
    end

    if settings.register == nil or settings.bit == nil then
        complain("buzzer.enabled is true but buzzer.register/buzzer.bit are not configured")
        return false
    end

    return true

end

-- One on/off cycle. Returns false when the hardware refused, so a caller can
-- tell "no buzzer configured" (true, nothing happened) from "the panel is down".
local function pulse(onMs, offMs)

    local ok, err

    if settings.backend == "zk" then
        -- One call, one pulse: zk.pulse latches the output and releases it
        -- after onMs. Two round trips to the controller, which is what
        -- millisecond control costs on this protocol.
        --
        -- The third argument is NOT optional here even when it is false:
        -- zk.pulse defaults `auxiliary` to TRUE, so leaving it out would put
        -- every beep back on an auxiliary output whatever this file says.
        local number, auxiliary = zkRelay()

        ok, err = zk.pulse(number, onMs, auxiliary)
    else
        ok, err = Modbus.trigger_bit(settings.register, settings.bit, onMs)
    end

    if not ok then
        Logger.warning("buzzer pulse failed: " .. tostring(err))
        return false
    end

    if offMs and offMs > 0 then
        Sleep(offMs)
    end

    return true

end

------------------------------------------------------------
-- Patterns
------------------------------------------------------------

function Buzzer.beep(count)

    count = count or 1

    if not usable() then
        Logger.debug("buzzer: " .. count .. " beep(s) (disabled)")
        return true
    end

    for index = 1, count do

        -- No trailing gap after the final beep: it would only delay the door.
        local gap = (index < count) and settings.beep_off_ms or 0

        if not pulse(settings.beep_on_ms or 150, gap) then
            return false
        end

    end

    return true

end

function Buzzer.long_beep()

    if not usable() then
        Logger.debug("buzzer: long beep (disabled)")
        return true
    end

    return pulse(settings.long_beep_ms or 1500, 0)

end

-- Opens the door relay on the ACCESS CONTROLLER, as opposed to the PLC output
-- the lockers use. Not part of the locker flow -- the doors here are PLC-driven
-- -- but the panel drives the room door on many installations, and the binding
-- exists, so the abstraction should not hide it.
function Buzzer.open_reader_door(door, seconds)

    if zk == nil then
        return false, "the zk_controller module is not available in this build"
    end

    return zk.openDoor(door or 1, seconds or 5)

end

------------------------------------------------------------
-- Named signals
------------------------------------------------------------

-- The vocabulary the plan defines. Access logic calls these, never beep(n)
-- directly, so changing what "wrong locker" sounds like is one edit here.

function Buzzer.granted()
    return Buzzer.beep(1)
end

-- Plan section 9: five beeps.
function Buzzer.wrong_locker()
    return Buzzer.beep(settings.wrong_type_beeps or Config.locker.wrong_type_beep_count or 5)
end

-- Plan section 10: one long beep.
function Buzzer.expired_card()
    return Buzzer.long_beep()
end

function Buzzer.denied()
    return Buzzer.beep(2)
end

function Buzzer.unknown_card()
    return Buzzer.beep(3)
end

-- Plan section 11: door open/close timeout.
function Buzzer.door_error()
    return Buzzer.beep(4)
end

-- CardScanPlan sections 3-7. The admin sequence needs feedback an operator can
-- count -- one beep per scan, so they know whether that presentation registered
-- -- and entering the mode needs a signal nothing else in the vocabulary makes.
-- Two long beeps is that signal: one long beep is already "expired card".
function Buzzer.admin_scan()
    return Buzzer.beep(1)
end

function Buzzer.admin_mode()

    if not usable() then
        Logger.debug("buzzer: admin mode (disabled)")
        return true
    end

    local long = settings.long_beep_ms or 1500

    if not pulse(long, settings.beep_off_ms or 150) then
        return false
    end

    return pulse(long, 0)

end

function Buzzer.admin_exit()
    return Buzzer.beep(2)
end

-- The contractor finished their day (CardScanPlan section 1, as revised: the
-- day ends on a scan sequence, not on the clock). One long beep then a short
-- one -- deliberately not any of the patterns above, because the difference
-- between "opened" and "opened, and that was the last time today" is the one an
-- operator most needs to hear at the cabinet.
function Buzzer.day_complete()

    if not usable() then
        Logger.debug("buzzer: day complete (disabled)")
        return true
    end

    if not pulse(settings.long_beep_ms or 1500, settings.beep_off_ms or 150) then
        return false
    end

    return pulse(settings.beep_on_ms or 150, 0)

end

function Buzzer.status()

    local number, auxiliary = zkRelay()

    return {
        enabled = settings.enabled == true,
        usable = usable(),
        backend = settings.backend,
        register = settings.register,
        bit = settings.bit,
        relay = number,
        relay_kind = auxiliary and "aux" or "user",
        aux_output = settings.aux_output,
        zk_available = zk ~= nil,
    }

end

return Buzzer
