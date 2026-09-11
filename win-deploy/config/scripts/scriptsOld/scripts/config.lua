-- config.lua
-- Global configuration. No business logic should exist in this file.

local Config = {}

------------------------------------------------------------
-- Employee Registration Server
------------------------------------------------------------

Config.SERVER_URL = "http://localhost:8091"
Config.API_KEY = "68661266cd61d81ff05604db68e787f4658b29b4ac5eadb3e270c3619dbf19f2"

------------------------------------------------------------
-- PLC (must match config.json's modbus.ip / modbus.port)
------------------------------------------------------------

Config.PLC_IP = "192.168.2.203"
Config.PLC_PORT = 502

-- Output coils (must match config.json's modbus.output_coil_start = 1280)
Config.OUTPUT_SCAN = 1280     -- X0: move card to scan position
Config.OUTPUT_OUT = 1280      -- X0: move ca0d out to resent to the user
Config.OUTPUT_IN = 1281      -- X1: move ca0d out to resent to the user
Config.OUTPUT_COLLECT = 1282  -- X2: collect/return the card

-- Inputs (must match config.json's modbus.input_coil_start = 1024). All four
-- are active-high: 1 means the condition is present.
Config.INPUT_CARD_TAKEN = 1024          -- sensor: user removed the card
Config.INPUT_CARD_SOURCE_LOW = 1025     -- card stock running low (still issuable)
Config.INPUT_CARD_REJECT_FULL = 1026    -- reject/collect bin full
Config.INPUT_CARD_NOT_IN_HOPPER = 1027  -- no card in the hopper at all

-- Which Modbus address space the inputs above actually live in.
--   "coil"     - read with FC01 (Read Coils)
--   "discrete" - read with FC02 (Read Discrete Inputs)
--   "auto"     - try FC02, fall back to FC01 if the PLC rejects it
--
-- These are two SEPARATE address spaces, and this PLC answers both at 1024:
-- the io_test scan reported "COILS 1024=true" but "DISCRETE INPUTS
-- 1024=false". Because both replies are valid, "auto" cannot tell which one
-- carries the live state -- it picks FC02 and the dashboard input then never
-- changes. Set this to whichever space actually tracks the sensor.
Config.INPUT_SOURCE = "discrete"

------------------------------------------------------------
-- ZK access controller (the card reader at the scan position)
------------------------------------------------------------

Config.ZK_IP = "192.168.1.201"
Config.ZK_PORT = 4370
Config.ZK_TIMEOUT = 2000

------------------------------------------------------------
-- Timeouts (milliseconds) and retry limits
------------------------------------------------------------

Config.CARD_WAIT_TIMEOUT = 5000
Config.TAKE_CARD_TIMEOUT = 30000
Config.SCAN_CARD_TIMEOUT = 1000

-- How long an error message stays on the LED panel before the workflow returns
-- to Idle. main.lua puts the idle prompt back the moment IssueCardWorkflow()
-- returns, so without this hold the message the person needs to act on ("card
-- already issued", "not registered") is overwritten within microseconds and
-- never appears at all.
Config.ERROR_HOLD_MS = 5000

Config.MAX_SCAN_RETRY = 3

-- How many employees a held card may be offered to before it is given up on.
-- A card nobody takes is kept inside the machine and re-offered to the next
-- employee instead of being binned, but the gateway only remembers the UID --
-- it cannot see whether the card is still physically there. Without a cap, a
-- card removed by hand (or a hold that silently failed) would be "reissued"
-- forever, and every employee would be told the reader is broken. At the cap it
-- goes to the collect bin and a fresh card is dispensed.
Config.MAX_CARD_REUSE = 3

------------------------------------------------------------
-- Machine status monitoring (hopper / reject bin / card stock)
------------------------------------------------------------

-- How often the idle loop re-reads the three status inputs. That loop spins
-- every 100ms and each check is three Modbus round trips, so status runs on its
-- own slower clock. Seconds, because os.time() is the only wall clock a script
-- has.
Config.STATUS_POLL_SEC = 2

-- An issue is reported to the server when it APPEARS and again when it CLEARS,
-- not on every poll -- otherwise a machine standing with a full reject bin would
-- post several times a second. While the condition persists it is re-sent this
-- often, so an alert that was missed (server down, ticket closed by mistake)
-- comes back instead of going quiet forever.
Config.STATUS_REPEAT_SEC = 300

------------------------------------------------------------
-- Misc
------------------------------------------------------------

Config.MACHINE_ID = "GW001"

return Config
