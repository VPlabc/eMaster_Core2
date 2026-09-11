-- api/server_api.lua
-- The backend REST server (Plan sections 23 and 26). Transport and shape only:
-- nothing here decides what a record means, and the caller always gets either a
-- validated list or an error -- never a half-parsed response.
--
-- Two things about the native binding shape this works around:
--
--   * Rest.Get() merges only the TOP-LEVEL SCALAR fields of a JSON object into
--     its result table. The active-card list is an array of objects, so it
--     arrives as .body text and is parsed here with utils/json.
--   * Rest.SetServer()/SetApiKey() mutate the gateway's shared RestClient,
--     which every other running script uses too. They are therefore called
--     only when smartlocker.json explicitly overrides the server, and requests
--     otherwise go out as absolute URLs (Rest.Get uses a URL containing "://"
--     as-is) or as plain paths against the gateway's configured server.

local Gateway = Rest

local Config = require("config")
local Json   = require("utils.json")
local Logger = require("utils.logger")

local Server = {}

local settings = Config.server

local configured = false

------------------------------------------------------------
-- Setup
------------------------------------------------------------

function Server.configure()

    -- An api_key only reaches the server through the shared RestClient; there
    -- is no per-request header binding. Setting it is therefore unavoidable
    -- when this project uses a different key from the rest of the gateway, and
    -- it is why overriding it is opt-in rather than the default.
    if settings.api_key ~= nil and settings.api_key ~= ""
       and settings.api_key ~= Config.GatewayValue("rest.api_key", "") then

        Logger.warning("server.api_key overrides the gateway's REST api key for ALL scripts in this process")
        Gateway.SetApiKey(settings.api_key)

    end

    configured = true

    return true

end

-- Absolute when server.base_url is set, a bare path otherwise (which Rest.Get
-- joins onto the gateway's configured REST url).
local function urlFor(path)

    local base = settings.base_url

    if base == nil or base == "" then
        return path
    end

    if path:find("://") then
        return path
    end

    local baseHasSlash = base:sub(-1) == "/"
    local pathHasSlash = path:sub(1, 1) == "/"

    if baseHasSlash and pathHasSlash then
        return base .. path:sub(2)
    end

    if not baseHasSlash and not pathHasSlash then
        return base .. "/" .. path
    end

    return base .. path

end

------------------------------------------------------------
-- Response handling
------------------------------------------------------------

-- Returns decoded body, or nil + a message that says which of the failure modes
-- of Plan section 23 happened: no connection, HTTP status, unparseable JSON.
local function request(path)

    if not configured then
        Server.configure()
    end

    local url = urlFor(path)

    local response = Gateway.Get(url)

    if response == nil then
        return nil, "no response from " .. url
    end

    if response.ok ~= true then
        -- Transport-level failure: connection refused, DNS, TLS, timeout. The
        -- gateway's RestClient reports the reason in .error.
        return nil, "request failed: " .. tostring(response.error or "no connection") .. " (" .. url .. ")"
    end

    local status = response.status or 0

    if status < 200 or status >= 300 then
        return nil, "HTTP " .. tostring(status) .. " from " .. url
    end

    if response.body == nil or response.body == "" then
        return nil, "empty response body from " .. url
    end

    local parsed, err = Json.decode(response.body)

    if parsed == nil then
        return nil, "invalid JSON from " .. url .. ": " .. tostring(err)
    end

    return parsed, nil, status

end

Server.request = request

-- Digs the array of records out of whatever envelope the server wrapped it in.
local function extractList(payload)

    if type(payload) ~= "table" then
        return nil, "response is not an object or array"
    end

    if #payload > 0 then
        return payload
    end

    for _, key in ipairs(settings.list_keys or {}) do

        local candidate = payload[key]

        if type(candidate) == "table" then

            if #candidate > 0 then
                return candidate
            end

            -- An explicitly empty list is a legitimate answer (every card
            -- revoked) and must not be mistaken for "wrong envelope key".
            if next(candidate) == nil then
                return {}
            end

        end

    end

    -- A server that reports failure in the body rather than the status line.
    if payload.success == false or payload.error ~= nil then
        return nil, tostring(payload.message or payload.error or "server reported failure")
    end

    return nil, "no card list found in the response (looked for: " ..
                table.concat(settings.list_keys or {}, ", ") .. ")"

end

-- Pulls one attribute out of a record, trying each configured field name in
-- turn. This is what lets the same code read servers that call the same thing
-- "name", "full_name" or "employee_name".
local function field(record, attribute)

    local names = (settings.fields or {})[attribute]

    if names == nil then
        return Json.value(record[attribute])
    end

    for _, name in ipairs(names) do

        local value = record[name]

        if value ~= nil then
            return Json.value(value)
        end

    end

    return nil

end

-- Server record -> the flat shape the rest of the project uses. Values are only
-- renamed and typed here; whether the result is *valid* is core/employee's
-- decision (Plan section 27).
function Server.normalize(record)

    if type(record) ~= "table" then
        return nil
    end

    local active = field(record, "active")

    if type(active) == "string" then
        local text = active:lower()
        active = (text == "true" or text == "1" or text == "active" or text == "enabled" or text == "yes")
    elseif type(active) == "number" then
        active = active ~= 0
    elseif active == nil then
        -- A list of ACTIVE cards that does not say so per record: absence means
        -- active, or the endpoint would return nothing usable.
        active = true
    end

    local expire = field(record, "expire_at")

    if expire == "" then
        expire = nil
    end

    -- CardScanPlan section 1's contractor_start_date. Optional everywhere: most
    -- servers send only an expiry, and a card with no start date is simply one
    -- that is already in its period.
    local start = field(record, "start_at")

    if start == "" then
        start = nil
    end

    local username = field(record, "username")
    local cardCode = field(record, "card_code")
    local role = field(record, "role")
    local gender = field(record, "gender")
    local lockerId = field(record, "locker_id")

    return {
        username = username and tostring(username) or nil,
        card_code = cardCode and tostring(cardCode) or nil,
        role = role and tostring(role):lower() or nil,
        gender = gender and tostring(gender):lower() or nil,
        expire_at = expire and tostring(expire) or nil,
        start_at = start and tostring(start) or nil,
        active = active == true,
        locker_id = tonumber(lockerId),
    }

end

------------------------------------------------------------
-- Endpoints
------------------------------------------------------------

-- Server.get_active_cards() -> list, error
--
-- The list is normalized but NOT filtered: a record the server marked inactive
-- still comes back, because the synchronisation has to deactivate the local
-- copy rather than simply not see it.
function Server.get_active_cards()

    local payload, err = request(settings.active_cards_path)

    if payload == nil then
        return nil, err
    end

    local list, listErr = extractList(payload)

    if list == nil then
        return nil, listErr
    end

    local out = {}
    local skipped = 0

    for _, record in ipairs(list) do

        local normalized = Server.normalize(record)

        if normalized ~= nil and normalized.card_code ~= nil and normalized.card_code ~= "" then
            out[#out + 1] = normalized
        else
            skipped = skipped + 1
        end

    end

    if skipped > 0 then
        Logger.warning("active card list: " .. skipped .. " record(s) skipped for having no card code")
    end

    return out

end

-- Server.get_employee(card_code) -> record, error
function Server.get_employee(cardCode)

    if cardCode == nil or cardCode == "" then
        return nil, "no card code given"
    end

    local payload, err = request(settings.employee_path .. tostring(cardCode))

    if payload == nil then
        return nil, err
    end

    -- Either the record itself, or wrapped the same way the list is.
    local record = payload

    for _, key in ipairs(settings.list_keys or {}) do
        if type(payload[key]) == "table" and payload[key].card_code ~= nil then
            record = payload[key]
            break
        end
    end

    local normalized = Server.normalize(record)

    if normalized == nil or normalized.card_code == nil then
        return nil, "response contained no card record"
    end

    return normalized

end

-- Cheap reachability probe for the status panel: any HTTP answer at all counts,
-- because a 404 still proves the server is up.
function Server.ping()

    local response = Gateway.Get(urlFor(settings.active_cards_path))

    if response == nil or response.ok ~= true then
        return false, response and response.error or "no response"
    end

    return true, response.status

end

return Server
