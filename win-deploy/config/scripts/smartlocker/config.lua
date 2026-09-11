-- config.lua
-- Loads smartlocker.json over a set of built-in defaults and hands the result
-- to every other module. No business logic lives here (Plan section 16: "must
-- be loaded by Lua and must not be hard-coded into business logic").
--
-- Three layers, later ones winning:
--
--   1. The defaults in this file -- a complete, runnable configuration, so a
--      fresh checkout starts even with no JSON file present.
--   2. The gateway's own settings (config.db), read through the native Config
--      binding: PLC address, ZK reader address, REST url and api key. These are
--      already configured on the Configuration page and there is no reason to
--      type them twice.
--   3. smartlocker.json -- everything specific to this application: locker
--      blocks, timings, sync interval.
--
-- The native global is captured as `Gateway` on the first line, before the
-- local `Config` below shadows it. Same split as the main gateway's config.lua:
-- addresses and credentials come from the gateway configuration; mechanical
-- timings, locker layout and business limits stay literal here.

local Gateway = Config

local Json  = require("utils.json")
local Paths = require("utils.paths")

local Config = {}

------------------------------------------------------------
-- Defaults
------------------------------------------------------------

local function defaults()

    return {

        system = {
            machine_id = "SL001",
            name = "SmartLocker Gateway",

            -- One pass of the main loop. 100 ms is the same clock the gateway's
            -- other workflow scripts run on: fast enough that a swipe feels
            -- instant, slow enough that door polling is a few Modbus reads a
            -- second rather than a flood.
            loop_interval_ms = 100,
        },

        plc = {
            host = "192.168.1.12",
            port = 502,

            -- Reads and writes go through the gateway's own configured Modbus
            -- connection (the `modbus` section of config.db). It is the only
            -- path that can write a holding register: the native
            -- Modbus.WriteHolding binding has no ip/port form, unlike
            -- ReadCoil/ReadDiscreteInput/ReadInputRegister. `host`/`port` above
            -- are then only checked against that configuration at start-up, and
            -- a mismatch is reported rather than silently obeyed.
            use_gateway_connection = true,

            -- "locker_io" -- THIS cabinet. Bit-addressed I/O: 10001 is the
            --                first lock relay coil (Y0), 20001 the first door
            --                sensor discrete input (X0), 30001/40001 unchanged.
            -- "modicon"   -- the classic map, where 10001 is a discrete input.
            --                For a cabinet wired the way Plan section 5.2
            --                assumed, with doors packed into registers.
            -- "raw"       -- the numbers are already protocol offsets. Cannot
            --                be combined with bit-addressed blocks: nothing is
            --                left to say which space an address belongs to, and
            --                hardware/modbus.lua says so at start-up.
            address_mode = "locker_io",

            -- Bit-addressed defaults, used by a block that gives no addresses
            -- of its own. One discrete input per door sensor from 20001, one
            -- coil per lock relay from 10001.
            input_space = "discrete_input",
            input_base = 20001,
            output_space = "coil",
            output_base = 10001,

            -- THIS CABINET IS BIT ADDRESSED, AND ONLY BIT ADDRESSED. Doors are
            -- coils (10001+) and discrete inputs (20001+); nothing here reads a
            -- door out of an input register (30001) or drives one from a
            -- holding register (40001).
            --
            -- The word-addressed mapping that used to live here -- sixteen
            -- doors packed into one register, Plan section 5 -- is gone. It was
            -- never how this panel is wired, and a fallback nobody uses is a
            -- second way for the addresses to be wrong. A block written in the
            -- old `register`/`start_bit` form is now a configuration error that
            -- says so, rather than quietly resolving to addresses the PLC does
            -- not answer for.
            --
            -- (30001/40001 still mean what they always meant to
            -- hardware/modbus.lua's address decoder -- the Modbus test client
            -- reads and writes them. They are simply not how a LOCKER is
            -- addressed.)

            -- Sensor polarity. 1 = the bit is set when the door stands OPEN.
            -- Section 5.1 requires this to be configurable because the wiring
            -- has not been verified yet; if every locker reads OPEN with the
            -- doors shut, set this to 0.
            door_open_value = 1,

            -- A read that fails is retried this many extra times before the
            -- operation is reported as an error. Covers a single dropped frame
            -- without turning a genuinely unreachable PLC into a long stall.
            retry_count = 1,
            retry_delay_ms = 50,

            -- Door polling asks about one locker at a time, but a register
            -- carries sixteen of them. With this on, every read of the same
            -- register within one pass of the main loop is answered from the
            -- first -- polling 36 lockers costs three Modbus round trips
            -- instead of thirty-six. Read-modify-write always bypasses it.
            read_cache = true,
        },

        locker = {
            lock_trigger_ms = 100,
            door_open_timeout = 30,
            door_close_timeout = 60,
            wrong_type_beep_count = 5,

            -- Set only when `blocks` is absent: a single employee block of this
            -- many lockers is synthesised from the plc.* addresses above, which
            -- is the shape of the example in Plan section 16.
            count = 18,

            -- An employee never gets a contractor locker unless this is on
            -- (Plan section 7).
            allow_cross_type = false,

            -- A valid card with no locker gets one at the moment it is
            -- presented, rather than being refused until the next
            -- synchronisation. Turn it off to make assignment a
            -- synchronisation-only decision.
            assign_on_scan = true,

            -- HOW A CONTRACTOR HOLDS A LOCKER. Two models, and a site picks
            -- one -- they answer the same question differently and running
            -- both would mean neither is predictable.
            --
            --   1  BY PERIOD (default). The locker is theirs for as long as
            --      their card is valid. It comes back when they have finished
            --      their last day AND their period has ended -- see
            --      auto_reclaim below.
            --
            --   2  BY HOLD. The locker is theirs for `hold_hours` from the
            --      moment it was assigned, and then it goes back in the pool
            --      for the next person whatever state it is in. This is the
            --      hot-desk model: a shared cabinet where nobody keeps a door
            --      overnight.
            --
            -- Mode 2 still honours the mode 1 rule as well, so somebody who
            -- finishes and expires inside their hold frees the door early
            -- rather than sitting on it for the rest of the 24 hours.
            contractor_mode = 1,

            -- Mode 2 only: how long a locker stays with the person it was
            -- assigned to, measured from lockers.assigned_at.
            hold_hours = 24,

            -- Take a contractor's locker back once they have BOTH finished
            -- their last day (the scan sequence -- see the `contractor`
            -- section) AND run out of card period. Both, never one:
            --
            --   completed + expired  -> reclaimed, the locker goes EMPTY
            --   expired, not completed -> left EXPIRED for a person to open
            --
            -- The second case is the whole reason this is safe to run on a
            -- timer. A contractor who never performed the finishing gesture may
            -- still have a bag in there, and no schedule should be the thing
            -- that decides otherwise; the admin override (CardScanPlan section
            -- 4) is how those are dealt with, by somebody who can look inside.
            auto_reclaim = true,

            -- With several cabinets of the SAME type, require the card to be
            -- presented at the cabinet its locker is actually in. Off by
            -- default: the type check already covers the mismatch Plan section
            -- 9 describes, and a site with one cabinet per type has no second
            -- block to confuse.
            enforce_block = false,

            -- Log a door that moves without an unlock (forced, or left ajar).
            report_unexpected_open = true,

            -- Both genders take the lowest free locker today (Plan section
            -- 29). Turning this on restricts women to a reserved range first,
            -- and only spills outside it when that range is full.
            female_priority = {
                enabled = false,
                start = 1,
                ["end"] = 6,
            },

            -- Locker layout. One form, bit addressed: `segments` lists one
            -- entry per physical I/O module, each with the address its first
            -- point answers at and how many points it carries. Door n of the
            -- block is the n-th point of the concatenated segments, so the gap
            -- between the PLC's internal I/O and the expansion module is simply
            -- two segments and never an address nobody wired.
            --
            -- Doors are COILS (10001+) and DISCRETE INPUTS (20001+). The
            -- word-addressed form -- `register`/`start_bit`, sixteen doors to a
            -- register -- has been removed; a block still written that way is
            -- reported through Config.LoadError() rather than resolved.
            --
            -- `output.count` is how many of the block's doors have an unlock
            -- output wired. A door past it reports its state and is never
            -- handed out, which is what section 6 describes for doors 7 and 8.
            blocks = {
                {
                    id = 1,
                    name = "Employee",
                    type = "contractor",
                    count = 18,
                    input = {
                        space = "discrete_input",
                        segments = {
                            { address = 20001, count = 6 },   -- PLC internal X0-X5
                            { address = 20017, count = 12 },  -- expansion module
                        },
                    },
                    output = {
                        space = "coil",
                        count = 18,
                        segments = {
                            { address = 10001, count = 6 },   -- PLC internal Y0-Y5
                            { address = 10017, count = 12 },  -- expansion module
                        },
                    },
                },
            },
        },

        -- CardScanPlan section 2. A contractor's card carries a period
        -- (start_at .. expire_at) from the server; these are the SHIFT HOURS
        -- inside a day of that period, and they are configuration for the same
        -- reason every other timing is: they are a fact about this site's
        -- working day, not about the software.
        --
        -- FINISHING THE DAY IS A GESTURE, NOT A TIME. A contractor opens their
        -- locker as often as they like -- every one of those opens is USING.
        -- The day is finished only when they say so: complete_scan_count scans
        -- in a row, close together. That is the difference between "I am done
        -- for today" and "I came back for my phone", and no clock can tell
        -- those apart.
        --
        -- So the windows below answer ONE question only: whether an open is
        -- allowed at all, and only when usage_timeout_enabled is true. They no
        -- longer decide whether an open completes the day.
        contractor = {
            morning_start   = "08:00",
            morning_end     = "09:30",

            afternoon_start = "17:00",
            afternoon_end   = "19:00",

            -- Consecutive scans of the SAME card that finish the day.
            complete_scan_count = 3,

            -- Seconds allowed BETWEEN those scans. The point of the limit is to
            -- tell a deliberate gesture from ordinary use: three opens spread
            -- across a shift are three opens, three scans in ten seconds are a
            -- decision. 0 removes the limit, which makes "any three opens
            -- today" finish the day instead.
            --
            -- Must stay comfortably above reader.repeat_ignore_ms (3 s), which
            -- drops a repeat of the same card -- otherwise the sequence cannot
            -- be completed at all, and init() says so at start-up.
            complete_scan_timeout = 10,

            -- Work past afternoon_end is still work. With this on, a late open
            -- is allowed and completes the day; with it off and
            -- usage_timeout_enabled on, it is refused.
            allow_ot = true,

            -- The hard gate described above. false = the windows only classify.
            usage_timeout_enabled = false,

            -- Refuse a contractor card before its start_at. The card is issued
            -- ahead of the job starting, and honouring it early would hand out
            -- a locker for a shift nobody is working yet. Cards with no
            -- start_at are unaffected -- most servers send only expire_at.
            enforce_start_date = true,

            -- How long an overtime extension pushed in by the server lasts.
            -- One day: tonight's overtime must not silently become tomorrow's
            -- normal finishing time (CardScanPlan section 2).
            overtime_expires_daily = true,
        },

        -- CardScanPlan sections 3-7. Admin cards are NOT employee records:
        -- they are recognised at the access-control layer before any lookup, so
        -- an admin card can never be handed a locker, expire, or be deactivated
        -- by a synchronisation that did not mention it.
        admin = {
            enabled = false,

            -- Card codes, as the reader reports them. Compared trimmed and
            -- upper case, the same normalisation every other card goes through.
            cards = {},

            -- Consecutive scans of an admin card that open ADMIN_ACCESS_MODE.
            unlock_all_scan_count = 5,

            -- Seconds allowed BETWEEN those scans. A card left on the reader,
            -- or an admin who walked away, falls back to NORMAL rather than
            -- leaving a half-entered sequence armed.
            scan_timeout = 10,

            -- Seconds ADMIN_ACCESS_MODE lasts, measured from the last thing
            -- that happened in it. Every admin action restarts the clock.
            override_timeout = 60,

            -- What an admin scan does once the mode is open. With it on, each
            -- further admin scan opens the NEXT expired locker -- one door per
            -- scan, never the whole cabinet at once (section 4: opening every
            -- latch simultaneously is what the plan explicitly rules out).
            -- With it off, an admin scan only extends the mode and expired
            -- lockers are opened from the dashboard instead.
            scan_opens_expired = true,

            -- Let an admin open a locker for someone whose own card is
            -- expired or inactive (section 5). This is the whole point of the
            -- mode; it is a switch only so a site can have the override
            -- without it.
            override_expired = true,
        },

        reader = {
            -- "zk"       -- the ZK access controller, via the in-process
            --               zk_controller module.
            -- "card_api" -- cards pushed in over POST /api/card/input, read
            --               from the gateway's single-slot Card cache.
            -- "both"     -- whichever produces a card first.
            source = "zk",

            host = "192.168.1.201",
            port = 4370,
            timeout_ms = 2000,
            password = "",

            -- How a swipe becomes the card_code stored in the database:
            --   "hex_msb"  -- Wiegand value widened by its parity bit and
            --                 printed as hex, the number printed on the card.
            --   "hex_lsb"  -- the same value, byte-reversed.
            --   "decimal"  -- the raw decimal the controller reports.
            --   "raw"      -- exactly what the reader sent, untouched.
            card_format = "hex_msb",
            reconstruct_parity = true,

            -- Whole-swipe de-bounce: the controller re-reports a card held
            -- against the reader, and each repeat would otherwise unlock the
            -- door again while the person is still opening it.
            repeat_ignore_ms = 3000,

            -- Which locker block the reader that produced the swipe belongs to
            -- (Plan section 8, "Determine Current Locker Block"). Keys are
            -- DoorID values from the controller, as strings. Anything not
            -- listed falls back to default_block; with a single cabinet that is
            -- the whole configuration.
            block_by_door = {},
            default_block = 1,

            -- Discards the controller's buffered backlog after connecting. It
            -- holds up to 30 realtime events and replays them on the first
            -- poll, so without this a swipe from yesterday opens a locker at
            -- start-up.
            flush_ms = 1500,
        },

        buzzer = {
            -- Nothing in the PullSDK addresses the reader's own sounder, so an
            -- audible signal is whatever relay the buzzer is wired to:
            --
            --   "zk"  an auxiliary output on the access controller
            --         (buzzer.aux_output), pulsed with zk.beep()
            --   "plc" a Modbus coil or register bit (buzzer.register/bit),
            --         pulsed like an unlock output
            --
            -- Off until one of them is actually wired. Every signal still
            -- succeeds while it is off; no access decision depends on it.
            enabled = false,
            backend = "zk",

            -- backend = "zk". Which relay on the controller the sounder is
            -- wired to, and which kind of relay that is:
            --   "user" -> a lock/door relay   (ControlDevice address type 1)
            --   "aux"  -> an auxiliary output (address type 2)
            --
            -- VERIFIED ON THIS CABINET with the Test Tools page: the sounder is
            -- on an AUXILIARY output, not a lock relay. The Test Tool plan
            -- (section 62) asked for a user relay; the hardware said otherwise
            -- and the hardware wins. This panel reports two of each, so the
            -- number is 1 or 2 -- found by listening, never assumed, because a
            -- wrong guess that lands on a LOCK relay opens a door instead of
            -- making a noise.
            relay = 1,
            relay_kind = "aux",

            -- Pre-section-62 spelling, still honoured. A file that sets only
            -- this keeps the old AUXILIARY meaning rather than being silently
            -- moved onto a lock relay -- see hardware/buzzer.lua's zkRelay().
            aux_output = nil,

            -- backend = "plc"
            register = 40001,
            bit = 15,

            beep_on_ms = 150,
            beep_off_ms = 150,
            long_beep_ms = 1500,
            wrong_type_beeps = 5,
        },

        server = {
            -- Empty means "use the gateway's configured REST server"
            -- (rest.url / rest.api_key), which is the normal case.
            base_url = "",
            api_key = "",

            active_cards_path = "/api/employees/active",
            employee_path = "/api/employees/",

            -- Field names accepted for each employee attribute, first match
            -- wins. Servers disagree on spelling and this is cheaper than a
            -- translation layer per deployment.
            fields = {
                username   = { "username", "name", "full_name", "employee_name" },
                card_code  = { "card_code", "card", "card_uid", "uid", "card_number" },
                role       = { "role", "type", "user_type", "employee_type" },
                gender     = { "gender", "sex" },
                expire_at  = { "expire_at", "expired_at", "expire_date", "valid_to", "expiry" },
                start_at   = { "start_at", "start_date", "contractor_start_date", "valid_from",
                               "effective_from", "begin_at" },
                active     = { "active", "is_active", "enabled", "status" },
                locker_id  = { "locker_id", "locker", "locker_number" },
            },

            -- Where the array of records sits in the response body. Checked in
            -- order; a bare JSON array is also accepted.
            list_keys = { "data", "cards", "employees", "results", "items" },
        },

        mq = {
            enabled = true,

            -- The broker connection, queue and bindings are configured in the
            -- gateway (config.db `mq` section) and owned by MqClient. Lua only
            -- drains the inbox -- there is no Lua-side connect.
            max_per_tick = 25,

            events = {
                created = "employee.card.created",
                updated = "employee.card.updated",
                revoked = "employee.card.revoked",
            },
        },

        sync = {
            interval = 86400,
            run_at_start = true,

            -- Retry delay after a failed daily sync. The gateway keeps serving
            -- cards from the local database meanwhile (Plan section 3.2).
            retry_interval = 900,

            -- A date-only expire_at covers the whole of that day.
            expire_at_end_of_day = true,

            -- What happens to a locker whose employee disappears from the
            -- server list: "release" frees it for the next person, "keep" only
            -- deactivates the card. Releasing is the reconciling behaviour the
            -- plan asks for; keep is the cautious one for a first rollout.
            on_removed = "release",

            -- Assign a locker to every new active card as it arrives. Off means
            -- assignment happens on first swipe instead.
            assign_on_sync = true,

            -- An empty active-card list is treated as a suspect response and
            -- deactivates nothing, because it is indistinguishable from a
            -- server that answered 200 with an empty envelope -- and acting on
            -- it would release every locker in the building. Set this true only
            -- for a deployment where "no cards" is a real state.
            allow_empty_list = false,
        },

        database = {
            -- SQLite, opened with Db.Open(). A relative path resolves against
            -- the gateway's CONFIG directory -- the same base the web UI's
            -- web.locker_db_path uses, so the dashboard finds this file with no
            -- second setting to keep in step.
            path = "smartlocker.db",

            -- Audit rows (locker_logs, system_logs) older than this are deleted
            -- once a day. 0 keeps everything; a cabinet doing 200 swipes a day
            -- grows by roughly that many rows.
            retention_days = 180,
        },

        logging = {
            structured = true,     -- Log.Write into config/logs.db
            recent_limit = 200,    -- in-memory ring the frontend snapshot reads
            debug = false,
        },

        frontend = {
            publish_interval_sec = 2,
            variable_prefix = "SL_",
        },

    }

end

------------------------------------------------------------
-- Merge
------------------------------------------------------------

local function isPlainTable(value)
    return type(value) == "table" and value ~= Json.null
end

-- Recursive merge, override winning. Arrays (the blocks list, the field-name
-- lists) are REPLACED rather than merged element-wise: a site that lists two
-- blocks means two blocks, not two blocks plus whichever default block happened
-- to occupy slot one.
local function merge(base, override)

    if not isPlainTable(override) then
        return override
    end

    if not isPlainTable(base) then
        return override
    end

    if #override > 0 or #base > 0 then
        return override
    end

    local result = {}

    for key, value in pairs(base) do
        result[key] = value
    end

    for key, value in pairs(override) do
        if value == Json.null then
            result[key] = nil
        else
            result[key] = merge(result[key], value)
        end
    end

    return result

end

------------------------------------------------------------
-- Gateway settings
------------------------------------------------------------

-- Reads a value from the gateway's own configuration, returning `fallback` when
-- the key is unset or the binding is unavailable (which is the case in a plain
-- `lua` interpreter, where these scripts are still worth being able to load).
local function gatewayValue(key, fallback)

    if type(Gateway) ~= "table" or type(Gateway.Get) ~= "function" then
        return fallback
    end

    local ok, value = pcall(Gateway.Get, key)

    if not ok or value == nil or value == "" then
        return fallback
    end

    return value

end

Config.GatewayValue = gatewayValue

------------------------------------------------------------
-- Locker layout expansion
------------------------------------------------------------

-- The word-addressed helpers that used to live here (bitAt / isBitAddressed,
-- sixteen doors to a register) are gone: this cabinet's doors are coils and
-- discrete inputs, and keeping a second addressing scheme that nothing uses is
-- keeping a second way to be wrong. See the plc section above.

-- Flattens a segment list into one address per door, in door order:
--
--   { { address = 20001, count = 6 }, { address = 20017, count = 12 } }
--     -> 20001..20006, 20017..20028
--
-- The point of the list is that the second module does NOT continue the first
-- one's numbering. Writing the doors out as a single run with a count would
-- put doors 7-12 at 20007-20012, addresses this PLC does not answer for.
local function expandSegments(map, defaultAddress, defaultCount)

    local segments = map.segments

    if type(segments) ~= "table" or #segments == 0 then
        segments = { { address = map.address or defaultAddress, count = map.count or defaultCount } }
    end

    local addresses = {}

    for _, segment in ipairs(segments) do

        local address = segment.address or segment.start

        if address ~= nil then
            for index = 0, (segment.count or 0) - 1 do
                addresses[#addresses + 1] = address + index
            end
        end

    end

    return addresses

end

-- The contiguous runs the addresses fall into, one entry per module, for
-- hardware/modbus.lua to read in a single FC01/FC02 instead of one round trip
-- per door. Built from the mapping itself rather than configured separately, so
-- there is no second place to keep an expansion module's base address correct.
local function bulkRanges(lockers)

    local bySpace = {}

    local function note(space, address)
        if space ~= nil and address ~= nil then
            bySpace[space] = bySpace[space] or {}
            bySpace[space][address] = true
        end
    end

    for _, locker in ipairs(lockers) do
        note(locker.input_space, locker.input_register)
        note(locker.output_space, locker.output_register)
    end

    local ranges = {}

    for space, set in pairs(bySpace) do

        local addresses = {}

        for address in pairs(set) do
            addresses[#addresses + 1] = address
        end

        table.sort(addresses)

        local first, previous = nil, nil

        local function flush()
            if first ~= nil and previous - first + 1 > 1 then
                ranges[#ranges + 1] = { space = space, address = first, count = previous - first + 1 }
            end
        end

        for _, address in ipairs(addresses) do

            if first == nil then
                first, previous = address, address
            elseif address == previous + 1 then
                previous = address
            else
                flush()
                first, previous = address, address
            end

        end

        flush()

    end

    -- Sorted so the list reads the same on every start-up; pairs() over the
    -- spaces above does not.
    table.sort(ranges, function(a, b)
        if a.space ~= b.space then
            return a.space < b.space
        end
        return a.address < b.address
    end)

    return ranges

end

-- Expands the configured blocks into one flat list of locker definitions, the
-- shape Plan section 6 asks each locker to resolve to.
local function expandBlocks(settings)

    local blocks = settings.locker.blocks

    if type(blocks) ~= "table" or #blocks == 0 then

        -- No blocks configured: one uninterrupted run of coils and discrete
        -- inputs from the plc bases -- the flat "count + plc addresses" form of
        -- the section 16 example. Correct only for a cabinet with no expansion
        -- module, since nothing here knows where a second one would start; a
        -- cabinet that has one describes itself with `segments`.
        local count = settings.locker.count or 6

        blocks = {
            {
                id = 1,
                name = "Locker",
                type = "employee",
                count = count,
                input  = { space = settings.plc.input_space,  address = settings.plc.input_base,  count = count },
                output = { space = settings.plc.output_space, address = settings.plc.output_base, count = count },
            },
        }

    end

    local lockers = {}
    local normalizedBlocks = {}
    local id = 0
    -- Returned to Config.Load, which surfaces it through Config.LoadError() --
    -- the same channel a broken smartlocker.json uses. A layout this file
    -- cannot honour has to be loud: the alternative is a cabinet quietly
    -- addressing the wrong points.
    local layoutError = nil

    for order, block in ipairs(blocks) do

        local input  = block.input  or {}
        local output = block.output or {}

        -- The old word-addressed form is refused rather than translated. A
        -- block still written that way describes a cabinet whose doors live in
        -- registers, and this gateway no longer addresses doors that way -- so
        -- resolving it to something would mean resolving it to the wrong thing,
        -- silently, at the addresses the PLC does not answer for.
        if input.register ~= nil or output.register ~= nil
            or input.start_bit ~= nil or output.start_bit ~= nil then

            layoutError = layoutError or
                ("locker block " .. tostring(block.id or order) ..
                 " uses the removed register/start_bit form; describe its doors with " ..
                 "`segments` of coils (10001+) and discrete inputs (20001+) instead")

        end

        -- Every door resolves to one address in one bit space, up front.
        local inputSpace = input.space or settings.plc.input_space or "discrete_input"
        local outputSpace = output.space or settings.plc.output_space or "coil"

        local inputAddresses = expandSegments(input, settings.plc.input_base, block.count)
        local outputAddresses = expandSegments(output, settings.plc.output_base,
                                                output.count or block.count)

        -- How many of this block's doors have an unlock output. Defaults to all
        -- of them, which is what this cabinet has; lower it for the section 6
        -- shape, where the last doors report their state and cannot be opened.
        local outputCount = output.count or block.count

        local blockId = block.id or order

        normalizedBlocks[#normalizedBlocks + 1] = {
            id = blockId,
            name = block.name or ("Block " .. tostring(blockId)),
            type = block.type or "employee",
            count = block.count or 0,
            output_count = outputCount,
            first_locker_id = id + 1,
        }

        for number = 1, (block.count or 0) do

            local inRegister, inBit, outRegister, outBit

            -- A coil and a discrete input each carry one bit, so the address is
            -- the whole mapping and the bit is 0. It is still recorded rather
            -- than left nil: `output_bit IS NOT NULL` is how the database says
            -- "this door can be opened", and the doors past output.count are
            -- the ones that must fail that test.
            inRegister = inputAddresses[number]
            inBit = inRegister ~= nil and 0 or nil

            outRegister = outputAddresses[number]
            outBit = outRegister ~= nil and 0 or nil

            if number > outputCount then
                outRegister, outBit = nil, nil
            end

            id = id + 1

            lockers[#lockers + 1] = {
                locker_id = id,
                block_id = blockId,
                block_name = block.name or ("Block " .. tostring(blockId)),
                locker_number = number,
                locker_type = block.type or "employee",
                input_register = inRegister,
                input_bit = inBit,
                output_register = outRegister,
                output_bit = outBit,

                -- Which address space those two numbers belong to. Not
                -- persisted: the locker rows in the database carry the
                -- addresses only, and hardware/modbus.lua recovers the space
                -- from the address itself. These are what
                -- Modbus.check_mapping() holds the addresses up against at
                -- start-up, and what the bulk-read runs below are grouped by.
                input_space = inputSpace,
                output_space = outRegister ~= nil and outputSpace or nil,
            }

        end

    end

    -- Explicit per-locker overrides, for a cabinet whose wiring does not follow
    -- the run (one swapped pair of doors should not force every address in the
    -- block to be spelled out).
    local overrides = settings.locker.overrides

    if type(overrides) == "table" then

        for _, override in ipairs(overrides) do

            for _, locker in ipairs(lockers) do

                local matches = (override.locker_id and override.locker_id == locker.locker_id)
                    or (override.block_id == locker.block_id and override.locker_number == locker.locker_number)

                if matches then
                    for key, value in pairs(override) do
                        if key ~= "locker_id" then
                            -- Json.null clears a field: that is how a locker
                            -- with no unlock output is spelled in an override.
                            if value == Json.null then
                                locker[key] = nil
                            else
                                locker[key] = value
                            end
                        end
                    end
                end

            end

        end

    end

    -- After the overrides, not before: a door moved to a different module moves
    -- the run that will be block-read for it.
    return lockers, normalizedBlocks, bulkRanges(lockers), layoutError

end

------------------------------------------------------------
-- Load
------------------------------------------------------------

local settings = nil
local sourcePath = nil
local loadError = nil

local function applyGatewayDefaults(values)

    values.plc.host = values.plc.host or gatewayValue("modbus.ip", nil)
    values.plc.port = values.plc.port or gatewayValue("modbus.port", 502)

    if values.reader.host == nil or values.reader.host == "" then
        values.reader.host = gatewayValue("zk.ip", values.reader.host)
    end

    if values.reader.port == nil then
        values.reader.port = gatewayValue("zk.port", 4370)
    end

    if values.reader.password == nil or values.reader.password == "" then
        values.reader.password = gatewayValue("zk.password", "")
    end

    if values.server.base_url == nil or values.server.base_url == "" then
        values.server.base_url = gatewayValue("rest.url", "")
    end

    if values.server.api_key == nil or values.server.api_key == "" then
        values.server.api_key = gatewayValue("rest.api_key", "")
    end

    if values.system.machine_id == nil or values.system.machine_id == "" then
        values.system.machine_id = gatewayValue("system.machine_id", "SL001")
    end

    return values

end

function Config.Load(explicitPath)

    local values = defaults()

    local candidates = explicitPath and { Paths.Resolve(explicitPath) } or Paths.ConfigCandidates()

    sourcePath = nil
    loadError = nil

    for _, candidate in ipairs(candidates) do

        local content = Paths.ReadFile(candidate)

        if content then

            local parsed, err = Json.decode(content)

            if parsed then
                -- "_comment" keys are documentation in the JSON file, the same
                -- convention config.example.json uses. Dropping them here keeps
                -- them out of every table the modules see.
                parsed._comment = nil
                values = merge(values, parsed)
                sourcePath = candidate
            else
                -- A broken configuration file must not take the defaults down
                -- with it, but it must be loud: this is the difference between
                -- "the site's timings" and "the shipped timings".
                loadError = "smartlocker.json parse error: " .. tostring(err) .. " (" .. candidate .. ")"
            end

            break

        end

    end

    values = applyGatewayDefaults(values)

    local layoutError

    values.lockers, values.blocks, values.bulk_ranges, layoutError = expandBlocks(values)

    -- A layout this file cannot honour is reported the same way a broken JSON
    -- file is, so main.lua's start-up check catches both. Not overwritten if the
    -- file itself already failed to parse: that is the more fundamental problem
    -- and the one worth naming first.
    if layoutError ~= nil and loadError == nil then
        loadError = layoutError
    end

    settings = values

    for key, value in pairs(values) do
        Config[key] = value
    end

    return settings

end

function Config.Reload()
    return Config.Load()
end

function Config.SourcePath()
    return sourcePath
end

function Config.LoadError()
    return loadError
end

-- Config.Value("locker.door_open_timeout" [, fallback])
function Config.Value(path, fallback)

    local node = settings

    for part in tostring(path):gmatch("[^%.]+") do

        if type(node) ~= "table" then
            return fallback
        end

        -- Indexed by name first, then by number, so "blocks.1.type" works.
        -- Written out rather than as `a or b` because a legitimately `false`
        -- setting (buzzer.enabled) would fall through an `or` to the fallback.
        local child = node[part]

        if child == nil then
            child = node[tonumber(part)]
        end

        if child == nil then
            return fallback
        end

        node = child

    end

    return node

end

-- These read the PUBLIC fields (Config.lockers / Config.blocks), not the
-- internal settings table, so a test that swaps in a different layout by
-- assigning Config.blocks is actually seen by the code under test.
function Config.Lockers()
    return Config.lockers or {}
end

function Config.Blocks()
    return Config.blocks or {}
end

-- The contiguous coil / discrete-input runs behind the configured lockers, one
-- per I/O module. hardware/modbus.lua block-reads these.
function Config.BulkRanges()
    return Config.bulk_ranges or {}
end

function Config.Block(blockId)

    for _, block in ipairs(Config.Blocks()) do
        if block.id == blockId then
            return block
        end
    end

    return nil

end

Config.Load()

-- Register the project schema without coupling business logic to persistence.
if config and config.register_schema then
    local schema = require("config_schema")
    for _, group in ipairs(schema.groups or {}) do
        for _, field in ipairs(group.fields or {}) do
            if field.key == "base_url" then field.default = Config.server.base_url end
            if field.key == "api_key" then field.default = Config.server.api_key end
            if field.key == "host" then field.default = Config.plc.host end
            if field.key == "port" then field.default = Config.plc.port end
            if field.key == "address_mode" then field.default = Config.plc.address_mode end
            if field.key == "read_cache" then field.default = Config.plc.read_cache end
            if field.key == "lock_trigger_ms" then field.default = Config.locker.lock_trigger_ms end
            if field.key == "door_open_timeout" then field.default = Config.locker.door_open_timeout end
            if field.key == "assign_on_scan" then field.default = Config.locker.assign_on_scan end
            if field.key == "allow_cross_type" then field.default = Config.locker.allow_cross_type end
        end
    end
    local ok, err = config.register_schema(schema)
    if not ok then
        print("SmartLocker dynamic config schema registration failed: " .. tostring(err))
    end
    Config.server.base_url = config.get("base_url") or Config.server.base_url
    Config.server.api_key = config.get("api_key") or Config.server.api_key
    Config.plc.host = config.get("host") or Config.plc.host
    Config.plc.port = config.get("port") or Config.plc.port
    Config.plc.address_mode = config.get("address_mode") or Config.plc.address_mode
    Config.plc.read_cache = config.get("read_cache")
    if Config.plc.read_cache == nil then Config.plc.read_cache = true end
    Config.locker.lock_trigger_ms = config.get("lock_trigger_ms") or Config.locker.lock_trigger_ms
    Config.locker.door_open_timeout = config.get("door_open_timeout") or Config.locker.door_open_timeout
    Config.locker.assign_on_scan = config.get("assign_on_scan")
    if Config.locker.assign_on_scan == nil then Config.locker.assign_on_scan = true end
    Config.locker.allow_cross_type = config.get("allow_cross_type")
    if Config.locker.allow_cross_type == nil then Config.locker.allow_cross_type = false end
end

function OnConfigChanged(project)
    if project ~= "smartlocker" or not config then return end
    Config.server.base_url = config.get("base_url") or Config.server.base_url
    Config.server.api_key = config.get("api_key") or Config.server.api_key
    Config.plc.host = config.get("host") or Config.plc.host
    Config.plc.port = config.get("port") or Config.plc.port
    Config.plc.address_mode = config.get("address_mode") or Config.plc.address_mode
    local readCache = config.get("read_cache")
    if readCache ~= nil then Config.plc.read_cache = readCache end
    Config.locker.lock_trigger_ms = config.get("lock_trigger_ms") or Config.locker.lock_trigger_ms
    Config.locker.door_open_timeout = config.get("door_open_timeout") or Config.locker.door_open_timeout
    local assignOnScan = config.get("assign_on_scan")
    if assignOnScan ~= nil then Config.locker.assign_on_scan = assignOnScan end
    local allowCrossType = config.get("allow_cross_type")
    if allowCrossType ~= nil then Config.locker.allow_cross_type = allowCrossType end
    Rest.SetServer(Config.server.base_url)
    Rest.SetApiKey(Config.server.api_key)
end

return Config
