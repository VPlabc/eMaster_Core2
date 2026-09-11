-- logger.lua
-- Unified logging interface. Every module should use this instead of the
-- native Log.* bindings directly, so the log source stays consistent if
-- logging behavior ever needs to change in one place.

local Logger = {}

function Logger.LogInfo(message)
    Log.Info(message)
end

function Logger.LogWarning(message)
    Log.Warning(message)
end

function Logger.LogError(message)
    Log.Error(message)
end

function Logger.LogDebug(message)
    Log.Debug(message)
end

return Logger
