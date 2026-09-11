-- utils.lua
-- Common helper functions shared across modules.

local Utils = {}

-- Every REST call in this workflow returns a table shaped like
-- { success, message, status }; this is the one place that knows that shape.
function Utils.IsSuccess(response)
    return response ~= nil and response.success == true
end

-- True when the server's reply mentions `needle`, case-insensitive substring.
--
-- Some outcomes are reported only in prose ("Employee already has an active
-- card") with no machine-readable code to switch on, so they have to be matched
-- as text. `body` is searched as well as `message`: the raw body always carries
-- the phrase even when the server puts it in a field name we don't know about,
-- or returns something PushRestResponseTable couldn't merge into the table.
function Utils.MessageContains(response, needle)

    if response == nil or needle == nil then
        return false
    end

    local text = string.lower(tostring(response.message or "") .. " " ..
                              tostring(response.error or "") .. " " ..
                              tostring(response.body or ""))

    return string.find(text, string.lower(needle), 1, true) ~= nil

end

function Utils.ResponseMessage(response, fallback)

    if response == nil then
        return fallback
    end

    if response.message and response.message ~= "" then
        return response.message
    end

    if response.status then
        return fallback .. ", status " .. tostring(response.status)
    end

    return fallback

end

return Utils
