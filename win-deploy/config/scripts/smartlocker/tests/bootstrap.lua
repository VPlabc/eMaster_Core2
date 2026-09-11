-- tests/bootstrap.lua
-- Makes require("core.access") and friends resolve from a test script.
--
-- LuaEngine puts two directories on package.path: the folder of the script
-- being run, and the scripts root. A test lives in smartlocker/tests/, so
-- neither of those is the project root -- this adds it.
--
-- Every test starts with:
--
--     local ok = pcall(require, "bootstrap")
--     if not ok then require("smartlocker.tests.bootstrap") end
--
-- The first form is the one that works when the test is started from the
-- Scripts list (its own folder is on the path). The second covers the Lua
-- Editor's Run button, which compiles the buffer as a string with no file
-- behind it, leaving only the scripts root on the path.

local function detectRoot()

    local source = debug.getinfo(1, "S").source or ""
    local dir = source:match("^@(.*)[/\\][^/\\]+$")

    if dir then
        -- .../smartlocker/tests -> .../smartlocker
        return (dir:match("^(.*)[/\\][^/\\]+$")) or dir
    end

    for entry in package.path:gmatch("[^;]+") do

        local base = entry:match("^(.*)%?%.lua$")

        if base then

            local candidate = base .. "smartlocker"
            local probe = io.open(candidate .. "/config.lua", "r")

            if probe then
                probe:close()
                return candidate
            end

        end

    end

    return nil

end

local root = detectRoot()

if root then
    package.path = root .. "/?.lua;" .. root .. "/?/init.lua;" .. package.path
end

return { root = root }
