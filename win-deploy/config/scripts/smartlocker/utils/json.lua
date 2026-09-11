-- utils/json.lua
-- Pure-Lua JSON encode/decode.
--
-- The gateway parses JSON for us only at the very edge: Rest.Get() merges the
-- TOP-LEVEL scalar fields of a response object into its result table, and
-- Mq.Get() hands back .body as text. Anything nested -- and the active-card
-- list is an array of objects -- arrives as a JSON *string*, so the scripts
-- need their own parser. There is no JSON binding in LuaEngine.
--
-- Design notes:
--   * decode() returns nil + message on malformed input. It never raises, so a
--     bad payload from the server or the broker is a handled error, never a
--     dead script (Plan section 36).
--   * JSON null decodes to Json.null, a unique sentinel, NOT to nil: a record
--     that says "expire_at": null has to be distinguishable from a record that
--     omits expire_at, and nil would also punch holes in arrays. Use
--     Json.value(x) to turn the sentinel back into nil once the distinction has
--     been made.
--   * encode() sorts object keys, so writing the same table twice produces the
--     same bytes -- which is what makes the on-disk database diffable and its
--     atomic rewrite cheap to eyeball.

local Json = {}

Json.null = setmetatable({}, { __tostring = function() return "null" end })

-- Json.null -> nil, everything else unchanged. Every read of a decoded field
-- that feeds business logic should go through this.
function Json.value(v)
    if v == Json.null then
        return nil
    end
    return v
end

------------------------------------------------------------
-- Encode
------------------------------------------------------------

local escapes = {
    ['"']  = '\\"',
    ['\\'] = '\\\\',
    ['\b'] = '\\b',
    ['\f'] = '\\f',
    ['\n'] = '\\n',
    ['\r'] = '\\r',
    ['\t'] = '\\t',
}

local function escapeString(s)

    return (s:gsub('[%z\1-\31\\"]', function(c)
        return escapes[c] or string.format('\\u%04x', string.byte(c))
    end))

end

-- Empty tables encode as {} rather than []. A locker record with no fields is
-- far more likely than an empty list in this project's data, and the decoder
-- treats both the same way on the way back in.
local function isArray(t)

    local count = 0

    for key in pairs(t) do
        if type(key) ~= "number" then
            return false
        end
        count = count + 1
    end

    for i = 1, count do
        if t[i] == nil then
            return false
        end
    end

    return count > 0

end

local function encodeNumber(value)

    if value ~= value or value == math.huge or value == -math.huge then
        -- NaN and infinity have no JSON representation; null is the honest
        -- answer and keeps the document parseable.
        return "null"
    end

    if math.type(value) == "integer" then
        return string.format("%d", value)
    end

    return string.format("%.14g", value)

end

local encodeValue

local function encodeTable(value, indent, level)

    if value == Json.null then
        return "null"
    end

    local newline, pad, padInner = "", "", ""

    if indent then
        newline = "\n"
        pad = string.rep(indent, level)
        padInner = string.rep(indent, level + 1)
    end

    if isArray(value) then

        local parts = {}

        for i = 1, #value do
            parts[#parts + 1] = padInner .. encodeValue(value[i], indent, level + 1)
        end

        if #parts == 0 then
            return "[]"
        end

        return "[" .. newline .. table.concat(parts, "," .. newline) .. newline .. pad .. "]"

    end

    local keys = {}

    for key in pairs(value) do
        if type(key) == "string" then
            keys[#keys + 1] = key
        elseif type(key) == "number" then
            keys[#keys + 1] = tostring(key)
        end
    end

    table.sort(keys)

    local parts = {}

    for _, key in ipairs(keys) do
        local raw = value[key] ~= nil and value[key] or value[tonumber(key) or key]
        parts[#parts + 1] = padInner .. '"' .. escapeString(key) .. '":' ..
                            (indent and " " or "") .. encodeValue(raw, indent, level + 1)
    end

    if #parts == 0 then
        return "{}"
    end

    return "{" .. newline .. table.concat(parts, "," .. newline) .. newline .. pad .. "}"

end

encodeValue = function(value, indent, level)

    local kind = type(value)

    if value == nil or value == Json.null then
        return "null"
    elseif kind == "boolean" then
        return value and "true" or "false"
    elseif kind == "number" then
        return encodeNumber(value)
    elseif kind == "string" then
        return '"' .. escapeString(value) .. '"'
    elseif kind == "table" then
        return encodeTable(value, indent, level)
    end

    -- Functions, userdata, threads: not representable. Encoding them as their
    -- tostring() would produce a document that decodes into nonsense, so they
    -- become null.
    return "null"

end

-- Json.encode(value [, pretty]) -> string
--
-- `pretty` true indents with two spaces, for files a human has to read (the
-- database snapshot, the example configuration). Omit it for anything going
-- over the wire.
function Json.encode(value, pretty)
    return encodeValue(value, pretty and "  " or nil, 0)
end

------------------------------------------------------------
-- Decode
------------------------------------------------------------

local Parser = {}
Parser.__index = Parser

local function newParser(text)
    return setmetatable({ text = text, pos = 1, len = #text }, Parser)
end

function Parser:error(message)
    error({ json = true, message = message .. " at position " .. self.pos }, 0)
end

function Parser:skipWhitespace()

    local _, stop = self.text:find("^[ \t\r\n]*", self.pos)

    if stop then
        self.pos = stop + 1
    end

end

function Parser:peek()
    return self.text:sub(self.pos, self.pos)
end

function Parser:expect(char)

    if self:peek() ~= char then
        self:error("expected '" .. char .. "' but found '" .. self:peek() .. "'")
    end

    self.pos = self.pos + 1

end

-- \uXXXX -> UTF-8. Surrogate pairs are joined, because a name coming back from
-- the server may well be encoded that way and the halves alone are not valid
-- characters.
function Parser:readUnicodeEscape()

    local hex = self.text:sub(self.pos, self.pos + 3)

    if not hex:match("^%x%x%x%x$") then
        self:error("invalid \\u escape")
    end

    self.pos = self.pos + 4
    local code = tonumber(hex, 16)

    if code >= 0xD800 and code <= 0xDBFF and self.text:sub(self.pos, self.pos + 1) == "\\u" then

        local lowHex = self.text:sub(self.pos + 2, self.pos + 5)

        if lowHex:match("^%x%x%x%x$") then

            local low = tonumber(lowHex, 16)

            if low >= 0xDC00 and low <= 0xDFFF then
                self.pos = self.pos + 6
                code = 0x10000 + (code - 0xD800) * 0x400 + (low - 0xDC00)
            end

        end

    end

    return utf8.char(code)

end

local stringEscapes = {
    ['"'] = '"', ['\\'] = '\\', ['/'] = '/',
    b = '\b', f = '\f', n = '\n', r = '\r', t = '\t',
}

function Parser:readString()

    self:expect('"')

    local parts = {}

    while true do

        local char = self:peek()

        if char == "" then
            self:error("unterminated string")
        elseif char == '"' then
            self.pos = self.pos + 1
            break
        elseif char == "\\" then

            self.pos = self.pos + 1
            local escape = self:peek()
            self.pos = self.pos + 1

            if escape == "u" then
                parts[#parts + 1] = self:readUnicodeEscape()
            elseif stringEscapes[escape] then
                parts[#parts + 1] = stringEscapes[escape]
            else
                self:error("invalid escape '\\" .. escape .. "'")
            end

        else
            local stop = self.text:find('[\\"]', self.pos)
            parts[#parts + 1] = self.text:sub(self.pos, (stop or self.len + 1) - 1)
            self.pos = stop or (self.len + 1)
        end

    end

    return table.concat(parts)

end

function Parser:readNumber()

    local literal = self.text:match("^%-?%d+%.?%d*[eE]?[%+%-]?%d*", self.pos)

    if not literal or literal == "" then
        self:error("invalid number")
    end

    local value = tonumber(literal)

    if value == nil then
        self:error("invalid number '" .. literal .. "'")
    end

    self.pos = self.pos + #literal

    -- Integers stay integers: an id or a locker number that decodes as 5.0
    -- would be written back out as 5.0 and stop matching anything.
    if math.type(value) == "float" and not literal:find("[%.eE]") then
        value = math.tointeger(value) or value
    end

    return value

end

local decodeValue

function Parser:readArray()

    self:expect("[")
    local result = {}

    self:skipWhitespace()

    if self:peek() == "]" then
        self.pos = self.pos + 1
        return result
    end

    while true do

        result[#result + 1] = decodeValue(self)
        self:skipWhitespace()

        local char = self:peek()

        if char == "," then
            self.pos = self.pos + 1
        elseif char == "]" then
            self.pos = self.pos + 1
            break
        else
            self:error("expected ',' or ']'")
        end

    end

    return result

end

function Parser:readObject()

    self:expect("{")
    local result = {}

    self:skipWhitespace()

    if self:peek() == "}" then
        self.pos = self.pos + 1
        return result
    end

    while true do

        self:skipWhitespace()
        local key = self:readString()

        self:skipWhitespace()
        self:expect(":")

        result[key] = decodeValue(self)
        self:skipWhitespace()

        local char = self:peek()

        if char == "," then
            self.pos = self.pos + 1
        elseif char == "}" then
            self.pos = self.pos + 1
            break
        else
            self:error("expected ',' or '}'")
        end

    end

    return result

end

decodeValue = function(parser)

    parser:skipWhitespace()

    local char = parser:peek()

    if char == "{" then
        return parser:readObject()
    elseif char == "[" then
        return parser:readArray()
    elseif char == '"' then
        return parser:readString()
    elseif char == "t" then
        if parser.text:sub(parser.pos, parser.pos + 3) ~= "true" then parser:error("invalid literal") end
        parser.pos = parser.pos + 4
        return true
    elseif char == "f" then
        if parser.text:sub(parser.pos, parser.pos + 4) ~= "false" then parser:error("invalid literal") end
        parser.pos = parser.pos + 5
        return false
    elseif char == "n" then
        if parser.text:sub(parser.pos, parser.pos + 3) ~= "null" then parser:error("invalid literal") end
        parser.pos = parser.pos + 4
        return Json.null
    elseif char == "" then
        parser:error("unexpected end of input")
    end

    return parser:readNumber()

end

-- Json.decode(text) -> value | nil, error
function Json.decode(text)

    if type(text) ~= "string" then
        return nil, "not a string"
    end

    -- A UTF-8 BOM ahead of the document is legal for a file and fatal for the
    -- parser; servers and editors both produce it.
    text = text:gsub("^\239\187\191", "")

    local parser = newParser(text)

    local ok, result = pcall(function()
        local value = decodeValue(parser)
        parser:skipWhitespace()
        if parser.pos <= parser.len then
            parser:error("trailing data")
        end
        return value
    end)

    if not ok then

        if type(result) == "table" and result.json then
            return nil, result.message
        end

        return nil, tostring(result)

    end

    return result

end

return Json
