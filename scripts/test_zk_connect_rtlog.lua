-- test_zk_connect_rtlog.lua
-- Bounded PASS/FAIL test: zk.connect() then confirm GetRTLog() polling is
-- actually happening (via onRawRTLog), not just that the connection came
-- up. Mirrors fastread_sdk's C++ self-test PASS/FAIL convention -- see
-- request/HSF_Machine_ZK_Controller_Lua_Integration.md section 20, Test 3
-- (TCP Connection) + the polling half of Test 5 (RTLog). Uses Log.Info/
-- Log.Error (checkable via GET /api/logs) rather than print(), unlike
-- test_zk_rtlog.lua which mirrors the spec's own print()-based example.

local zk = require("zk_controller")

local ZK_IP = "10.0.0.237"
local ZK_PORT = 4370
local ZK_TIMEOUT_MS = 2000

local WAIT_MS = 10000
local POLL_MS = 200

local failed = false
local rawCount = 0

local function Check(ok, passMessage, failMessage)
    if ok then
        Log.Info("test_zk_connect_rtlog PASS: " .. passMessage)
    else
        Log.Error("test_zk_connect_rtlog FAIL: " .. failMessage)
        failed = true
    end
    return ok
end

zk.onRawRTLog(function(raw)
    rawCount = rawCount + 1
end)

Log.Info("test_zk_connect_rtlog: connecting to " .. ZK_IP .. ":" .. ZK_PORT .. " ...")

local ok, err = zk.connect(ZK_IP, ZK_PORT, ZK_TIMEOUT_MS, "")

if not Check(ok, "zk.connect() succeeded", "zk.connect() failed: " .. tostring(err)) then
    Log.Error("test_zk_connect_rtlog: FAIL")
    return
end

Check(zk.isConnected(), "zk.isConnected() true after connect",
    "zk.isConnected() false right after a successful connect")

zk.startRTLog()
Log.Info("test_zk_connect_rtlog: waiting up to " .. WAIT_MS .. "ms for GetRTLog() polling data...")

local elapsed = 0
while elapsed < WAIT_MS and rawCount == 0 do
    Sleep(POLL_MS)
    elapsed = elapsed + POLL_MS
end

Check(rawCount > 0, rawCount .. " onRawRTLog callback(s) received -- GetRTLog() polling confirmed",
    "no onRawRTLog callback received within " .. WAIT_MS .. "ms (polling not happening, or the " ..
        "controller returned nothing at all -- ret<=0 every attempt)")

zk.stopRTLog()
zk.disconnect()

Check(not zk.isConnected(), "zk.isConnected() false after disconnect",
    "zk.isConnected() still true after disconnect")

if failed then
    Log.Error("test_zk_connect_rtlog: FAIL")
else
    Log.Info("test_zk_connect_rtlog: PASS")
end
