-- utils/paths.lua
-- Where this project's files live on disk.
--
-- Nothing here may assume a working directory. hsf_gateway.exe is started from
-- wherever the operator happens to be (a service, a shortcut, a shell in
-- build/), so a relative io.open("data/smartlocker.json") lands in a different
-- place every time -- and silently creates a second, empty database when it
-- guesses wrong. Every path this project touches is resolved from the location
-- of the Lua sources instead.

local Paths = {}

local separator = package.config:sub(1, 1)

local function normalize(path)
    if separator == "\\" then
        return (path:gsub("/", "\\"))
    end
    return (path:gsub("\\", "/"))
end

function Paths.Join(...)

    local parts = { ... }
    local out = {}

    for _, part in ipairs(parts) do
        if part ~= nil and part ~= "" then
            out[#out + 1] = (tostring(part):gsub("[/\\]+$", ""))
        end
    end

    return normalize(table.concat(out, "/"))

end

------------------------------------------------------------
-- Project root
------------------------------------------------------------

-- Two ways to find it, in order:
--
--   1. This file's own source path. Correct whenever the script was loaded
--      from a file, which is every case that matters.
--   2. The package.path entries LuaEngine installed (the script's own folder
--      and the scripts root), with "smartlocker" appended. This is the fallback
--      for a chunk with no file behind it -- the Lua Editor's Run button
--      compiles the buffer as a string, and then debug.getinfo has nothing to
--      report.
local function detectRoot()

    local source = debug.getinfo(1, "S").source or ""
    local dir = source:match("^@(.*)[/\\][^/\\]+$")

    if dir then
        -- .../smartlocker/utils -> .../smartlocker
        local root = dir:match("^(.*)[/\\][^/\\]+$")
        if root then
            return normalize(root)
        end
    end

    for entry in package.path:gmatch("[^;]+") do
        local base = entry:match("^(.*)%?%.lua$")
        if base then
            local candidate = Paths.Join(base, "smartlocker")
            local probe = io.open(Paths.Join(candidate, "config.lua"), "r")
            if probe then
                probe:close()
                return candidate
            end
        end
    end

    return "."

end

local root = detectRoot()

function Paths.Root()
    return root
end

-- Lets a test or an embedder point the project at a different tree without
-- editing this file.
function Paths.SetRoot(path)
    root = normalize(path)
end

function Paths.Resolve(path)

    if path == nil or path == "" then
        return nil
    end

    -- Already absolute (/x, C:\x, \\server\share): use as given.
    if path:match("^[/\\]") or path:match("^%a:[/\\]") then
        return normalize(path)
    end

    return Paths.Join(root, path)

end

------------------------------------------------------------
-- Configuration file lookup
------------------------------------------------------------

-- Plan section 16 recommends /config/smartlocker.json; keeping a copy beside
-- the scripts is what makes the project self-contained and copyable. Both are
-- supported, project-local first, so a site can override the shipped defaults
-- from the gateway's config directory without touching the source tree.
function Paths.ConfigCandidates()
    return {
        Paths.Join(root, "smartlocker.json"),
        Paths.Join(root, "..", "..", "smartlocker.json"),   -- config/smartlocker.json
    }
end

------------------------------------------------------------
-- Directories
------------------------------------------------------------

function Paths.Exists(path)

    local handle = io.open(path, "r")

    if handle then
        handle:close()
        return true
    end

    return false

end

-- Lua has no mkdir. os.execute is the only portable-ish way, and it is called
-- at most once per run: the common case is that the directory already exists,
-- and the probe below costs one failed io.open.
function Paths.EnsureDir(path)

    if path == nil or path == "" then
        return false
    end

    local probe = Paths.Join(path, ".dir_probe")
    local handle = io.open(probe, "w")

    if handle then
        handle:close()
        os.remove(probe)
        return true
    end

    if separator == "\\" then
        os.execute('mkdir "' .. normalize(path) .. '" 2>NUL')
    else
        os.execute('mkdir -p "' .. path .. '" 2>/dev/null')
    end

    handle = io.open(probe, "w")

    if handle then
        handle:close()
        os.remove(probe)
        return true
    end

    return false

end

function Paths.DirName(path)
    return (normalize(path):match("^(.*)[/\\][^/\\]*$")) or "."
end

------------------------------------------------------------
-- Whole-file read / atomic write
------------------------------------------------------------

function Paths.ReadFile(path)

    local handle, err = io.open(path, "rb")

    if not handle then
        return nil, err or ("cannot open " .. tostring(path))
    end

    local content = handle:read("a")
    handle:close()

    return content

end

-- Write to a sibling temporary file, then rename over the target. A gateway
-- killed mid-save then leaves either the old database or the new one -- never a
-- half-written file that fails to parse and takes every locker assignment with
-- it.
function Paths.WriteFileAtomic(path, content)

    local tmp = path .. ".tmp"

    local handle, err = io.open(tmp, "wb")

    if not handle then
        return false, err or ("cannot write " .. tostring(tmp))
    end

    local ok, writeErr = handle:write(content)
    handle:close()

    if not ok then
        os.remove(tmp)
        return false, writeErr or "write failed"
    end

    -- Windows' rename refuses to replace an existing file, so the old one goes
    -- first. The window this opens is the reason for the .bak below: if the
    -- process dies between the remove and the rename, the previous contents are
    -- still recoverable by hand.
    if Paths.Exists(path) then
        os.remove(path .. ".bak")
        os.rename(path, path .. ".bak")
    end

    local renamed, renameErr = os.rename(tmp, path)

    if not renamed then
        return false, renameErr or "rename failed"
    end

    return true

end

return Paths
