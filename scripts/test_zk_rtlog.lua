-- test_zk_rtlog.lua
-- Direct in-process ZK controller test (no bridge process) -- see
-- request/HSF_Machine_ZK_Controller_Lua_Integration.md section 17/20.
-- Run this from the Lua Editor (or via zk.connect()'s ok/err return) while
-- pointed at a real controller; present a card to it and watch for the
-- CARD EVENT block below.

local zk = require("zk_controller")

print("=== ZK RTLog Test ===")

zk.onConnectionChanged(function(connected)

    if connected then
        print("[ZK] Connected")
    else
        print("[ZK] Disconnected")
    end

end)

zk.onRawRTLog(function(raw)

    print("[ZK RAW]")
    print(raw)

end)

zk.onCard(function(event)

    print("")
    print("========== CARD EVENT ==========")

    print("CardData :", event.CardData)
    print("DoorID   :", event.DoorID)
    print("ReaderID :", event.ReaderID)
    print("InOutStatus:", event.InOutStatus)

    print("EventType:", event.EventType)
    print("VerifyMode:", event.VerifyMode)

end)

zk.setAutoReconnect(true)

local ok, err = zk.connect(
    "10.0.0.237",
    4370,
    2000,
    ""
)

if not ok then
    print("Connection failed:", err)
    return
end

zk.startRTLog()

print("RTLog monitoring started.")
