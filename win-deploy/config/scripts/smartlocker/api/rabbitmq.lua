-- api/rabbitmq.lua
-- Realtime employee/card updates from the broker (Plan sections 3.2 and 24).
--
-- The gateway's MqClient owns the AMQP connection, the queue, the bindings and
-- the acks; it runs on its own IO thread and reconnects by itself. What Lua
-- gets is an inbox: Mq.Available(), Mq.Get(), Mq.Status(), Mq.Publish(). So
-- "connect" here means "check that the broker side is up", and "subscribe"
-- means "register the handler this module calls for each decoded event" -- the
-- queue itself is configured on the gateway's Configuration page, not from a
-- script.
--
-- A message is acked by the C++ side as soon as it lands in the inbox, so a
-- script that stops draining loses no messages to redelivery -- but it does
-- fall behind. RabbitMQ.process() is therefore called every pass of the main
-- loop and drains up to mq.max_per_tick messages, bounded so a backlog cannot
-- stall the card reader.
--
-- Nothing in the access path depends on this module (Plan rule 6): a broker
-- that is down delays updates, it does not stop a locker from opening.

local Gateway = Mq

local Config = require("config")
local Json   = require("utils.json")
local Logger = require("utils.logger")

local RabbitMQ = {}

local settings = Config.mq

local handler = nil
local stats = { received = 0, handled = 0, ignored = 0, invalid = 0 }
local warnedDisabled = false

------------------------------------------------------------
-- Connection
------------------------------------------------------------

function RabbitMQ.available()
    return type(Gateway) == "table" and type(Gateway.Status) == "function"
end

function RabbitMQ.status()

    if not RabbitMQ.available() then
        return { enabled = false, connected = false, error = "Mq binding is not available in this build" }
    end

    return Gateway.Status()

end

function RabbitMQ.connect()

    if not settings.enabled then
        return false, "mq.enabled is false in smartlocker.json"
    end

    local status = RabbitMQ.status()

    if status.enabled ~= true then
        return false, status.error or "RabbitMQ is disabled in the gateway configuration"
    end

    if status.connected ~= true then
        -- Not fatal and not final: MqClient reconnects on its own, so this is
        -- reported once and the loop keeps calling process().
        return false, status.error or "broker is not connected yet"
    end

    return true

end

function RabbitMQ.is_connected()

    local status = RabbitMQ.status()

    return status.connected == true

end

function RabbitMQ.disconnect()

    -- Pause() stops the consumer without tearing the connection down, which is
    -- the closest thing to "unsubscribe" the binding offers.
    if RabbitMQ.available() and type(Gateway.Pause) == "function" then
        Gateway.Pause(true)
    end

    return true

end

-- RabbitMQ.subscribe(fn) -- fn(eventName, data, message) is called once per
-- decoded event. `queue` is accepted and ignored: the queue is the gateway's,
-- and taking it here would suggest a script could change it.
function RabbitMQ.subscribe(fn, queue)

    handler = fn

    if queue ~= nil then
        Logger.warning("RabbitMQ.subscribe: the queue is configured in the gateway (mq section), " ..
                       "not from Lua -- '" .. tostring(queue) .. "' ignored")
    end

    return true

end

------------------------------------------------------------
-- Message decoding
------------------------------------------------------------

local function eventNames()

    local events = settings.events or {}

    return {
        [events.created or "employee.card.created"] = "created",
        [events.updated or "employee.card.updated"] = "updated",
        [events.revoked or "employee.card.revoked"] = "revoked",
    }

end

-- Accepts the two shapes a server realistically sends:
--
--   { "event": "employee.card.created", "data": { ... } }   (Plan section 3.2)
--   a bare card record, with the event name in the routing key
--
-- Returns eventName, data or nil + reason.
function RabbitMQ.decode(message)

    if message == nil then
        return nil, "empty message"
    end

    local text = message.body

    if text == nil or text == "" then
        text = message.raw
    end

    if text == nil or text == "" then
        return nil, "message has no body"
    end

    local payload, err = Json.decode(text)

    if payload == nil then
        return nil, "invalid JSON: " .. tostring(err)
    end

    if type(payload) ~= "table" then
        return nil, "payload is not an object"
    end

    local name = payload.event or payload.type or payload.action or message.routing_key
    local data = payload.data or payload.payload or payload.card or payload.employee

    if data == nil and payload.card_code ~= nil then
        data = payload
    end

    if name == nil then
        return nil, "message carries no event name"
    end

    if type(data) ~= "table" then
        return nil, "event '" .. tostring(name) .. "' carries no data object"
    end

    return tostring(name), data

end

------------------------------------------------------------
-- Draining the inbox
------------------------------------------------------------

-- Called every pass of the main loop. Returns the number of messages handled.
function RabbitMQ.process(limit)

    if not settings.enabled then
        return 0
    end

    if not RabbitMQ.available() then

        if not warnedDisabled then
            warnedDisabled = true
            Logger.warning("RabbitMQ: the Mq binding is missing from this build; realtime updates are off. " ..
                           "The daily REST synchronisation still reconciles the database.")
        end

        return 0

    end

    local budget = limit or settings.max_per_tick or 25
    local handled = 0

    while handled < budget and Gateway.Available() do

        local message = Gateway.Get()

        if message == nil then
            break
        end

        stats.received = stats.received + 1
        handled = handled + 1

        local name, data = RabbitMQ.decode(message)

        if name == nil then

            stats.invalid = stats.invalid + 1
            Logger.warning("RabbitMQ: discarding a message -- " .. tostring(data))

        else

            local known = eventNames()[name]

            if known == nil then

                -- Another application's traffic on a shared exchange. Counted,
                -- not logged per message.
                stats.ignored = stats.ignored + 1

            elseif handler ~= nil then

                local ok, err = pcall(handler, name, data, message)

                if ok then
                    stats.handled = stats.handled + 1
                else
                    stats.invalid = stats.invalid + 1
                    Logger.system("RABBITMQ_HANDLER_FAILED",
                                  "RabbitMQ handler raised an error on '" .. name .. "'",
                                  tostring(err), "ERROR")
                end

            end

        end

    end

    return handled

end

function RabbitMQ.stats()
    return stats
end

-- Publishing back to the broker is not part of the plan's flow, but the
-- gateway offers it and a deployment that wants to report locker events
-- upstream should not have to reach around this module for it.
function RabbitMQ.publish(payload, routingKey)

    if not RabbitMQ.available() then
        return false, "Mq binding is not available"
    end

    local text = (type(payload) == "string") and payload or Json.encode(payload)

    return Gateway.Publish(text, routingKey)

end

return RabbitMQ
