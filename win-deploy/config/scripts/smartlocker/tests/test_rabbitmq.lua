-- tests/test_rabbitmq.lua
-- Stage 2.1 (Plan section 24): realtime updates from the broker.
--
-- The connection, the queue and the bindings belong to the gateway's MqClient
-- and are configured on the Configuration page -- there is no Lua-side connect
-- to test. What is tested here is everything downstream of that: that messages
-- arrive, that the three event names decode, and that a malformed payload is
-- rejected instead of taken seriously.
--
-- The decode checks run with no broker at all, so this test is still worth
-- running on a bench.

local ok = pcall(require, "bootstrap")
if not ok then require("smartlocker.tests.bootstrap") end

local Config   = require("config")
local Harness  = require("tests.harness")
local Json     = require("utils.json")
local RabbitMQ = require("api.rabbitmq")

local LISTEN_SECONDS = 60

Harness.start("Stage 2.1 -- RabbitMQ")

local events = Config.mq.events or {}

Harness.info("created: " .. tostring(events.created))
Harness.info("updated: " .. tostring(events.updated))
Harness.info("revoked: " .. tostring(events.revoked))

------------------------------------------------------------
-- Decoding (no broker needed)
------------------------------------------------------------

Harness.section("Message decoding")

local samples = {

    {
        name = "created, plan section 3.2 shape",
        body = Json.encode({
            event = events.created,
            data = { username = "NGUYEN VAN A", card_code = "ABCD0123", role = "employee",
                     gender = "female", expire_at = Json.null, active = true },
        }),
        expect = events.created,
    },

    {
        name = "updated",
        body = Json.encode({
            event = events.updated,
            data = { username = "NGUYEN VAN B", card_code = "ABCD5678", role = "contractor",
                     gender = "male", expire_at = "2026-12-31", active = true },
        }),
        expect = events.updated,
    },

    {
        name = "revoked",
        body = Json.encode({ event = events.revoked, data = { card_code = "ABCD5678" } }),
        expect = events.revoked,
    },

    {
        name = "event name in the routing key, bare record body",
        body = Json.encode({ card_code = "ABCD9999", role = "employee", active = true }),
        routing_key = events.created,
        expect = events.created,
    },

}

for _, sample in ipairs(samples) do

    local name, data = RabbitMQ.decode({ body = sample.body, routing_key = sample.routing_key })

    Harness.check("decode: " .. sample.name, name == sample.expect,
                  name == nil and tostring(data) or ("got " .. tostring(name)))

    if name ~= nil then
        Harness.check("  -- carries a card record", type(data) == "table" and data.card_code ~= nil)
    end

end

local badSamples = {
    { name = "not JSON",          message = { body = "<html>502 Bad Gateway</html>" } },
    { name = "empty body",        message = { body = "" } },
    { name = "no event name",     message = { body = '{"data":{"card_code":"X"}}' } },
    { name = "no data object",    message = { body = '{"event":"' .. tostring(events.created) .. '"}' } },
}

for _, sample in ipairs(badSamples) do

    local name, reason = RabbitMQ.decode(sample.message)

    Harness.check("rejected: " .. sample.name, name == nil, reason)

end

------------------------------------------------------------
-- The broker itself
------------------------------------------------------------

Harness.section("Broker")

local status = RabbitMQ.status()

Harness.info("enabled:   " .. tostring(status.enabled))
Harness.info("connected: " .. tostring(status.connected))

if status.error then
    Harness.info("error:     " .. tostring(status.error))
end

if status.enabled ~= true then

    Harness.skip("live messages", "RabbitMQ is disabled in the gateway configuration")

    Harness.info("")
    Harness.info("Enable it in the gateway's mq configuration section. The daily REST")
    Harness.info("synchronisation reconciles the database without it (Plan section 3.2).")

    Harness.finish()
    return

end

local connected, connectError = RabbitMQ.connect()

Harness.check("broker connected", connected, connectError)

if status.queue then
    Harness.info("queue:     " .. tostring(status.queue) .. "   exchange: " .. tostring(status.exchange))
end

------------------------------------------------------------
-- Listen
------------------------------------------------------------

Harness.section("Listening for " .. LISTEN_SECONDS .. "s -- create/update/revoke a card on the server")

local received = {}

RabbitMQ.subscribe(function(name, data)

    received[#received + 1] = { name = name, card = data.card_code }

    Harness.info("received " .. tostring(name) .. " for " .. tostring(data.card_code))

end)

local elapsed = 0

while elapsed < LISTEN_SECONDS * 1000 do
    RabbitMQ.process()
    Sleep(200)
    elapsed = elapsed + 200
end

local stats = RabbitMQ.stats()

Harness.info(string.format("received=%d handled=%d ignored=%d invalid=%d",
                           stats.received, stats.handled, stats.ignored, stats.invalid))

Harness.check("at least one event arrived", #received > 0,
              "zero means the server published nothing, or the queue binding does not match the routing keys")

Harness.check("no message was rejected as malformed", stats.invalid == 0)

Harness.info("")
Harness.info("Checklist for Plan section 24:")
Harness.info("  [ ] created / updated / revoked all arrive")
Harness.info("  [ ] restarting the broker recovers on its own (MqClient reconnects)")
Harness.info("  [ ] messages for other applications are ignored, not rejected")

Harness.finish()
