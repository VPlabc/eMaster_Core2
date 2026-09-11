-- zkcard_test.lua
-- Exercises zkcard.lua the same way workflow.lua's scan step does: connect
-- from inside a running script, then wait for a card. Bounded, so it always
-- returns instead of looping like ZK_Test.lua.
--
-- Must be run as a FILE (Lua Editor / POST /api/lua/scripts/zkcard_test.lua/run)
-- rather than pasted into /api/lua/run -- require() only resolves sibling
-- modules for scripts run from a file (see LuaEngine's scriptDir_).

local ZkCard = require("zkcard")

Log.Info("zkcard_test: calling ZkCard.WaitCard (connects from inside a running script)")

local r = ZkCard.WaitCard(8000)

if r.ok then
    Log.Info("zkcard_test: card read" ..
        " uid=" .. tostring(r.uid) ..
        " raw=" .. tostring(r.raw) ..
        " raw_hex=0x" .. tostring(r.raw_hex) ..
        " lsb=0x" .. tostring(r.uid_lsb) ..
        " door=" .. tostring(r.door) ..
        " reader=" .. tostring(r.reader))
else
    Log.Info("zkcard_test: no card within timeout (expected if nothing was swiped)" ..
        " error=" .. tostring(r.error))
end

Log.Info("zkcard_test: connected=" .. tostring(ZkCard.IsConnected()))
Log.Info("zkcard_test: DONE -- returned cleanly, no deadlock")
