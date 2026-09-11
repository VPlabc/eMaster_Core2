-- io_test.lua
-- PLC I/O test: reads discrete inputs (FC02) and coils (FC01), registers a
-- few named points for the Dashboard, and scans for addresses this PLC
-- actually accepts.
--
-- Why the scan: a Modbus exception 0x02 means "illegal data address" -- the
-- PLC answered, it simply has nothing at that address. Vendors map I/O to
-- wildly different bases (0, 1, 1024, 8192, 10000...), so guessing is
-- futile; this asks the PLC directly and reports what it accepts.

local SCAN_BASES = { 0, 1, 100, 1000, 1024, 2048, 4096, 8192, 10000, 10001 }

------------------------------------------------------------
-- Dashboard I/O map
------------------------------------------------------------

-- Registers the SAME address twice, once per address space, so the dashboard
-- shows both side by side. Whichever row actually tracks the sensor tells you
-- what to pass as the third argument in plc.lua / config.lua -- the other one
-- will sit at a constant value.
Modbus.RegisterInput("Card Taken (disc FC02)", 1024, "discrete")
Modbus.RegisterInput("Card Low (disc FC02)", 1025, "discrete")
Modbus.RegisterInput("Card Reject full (disc FC02)", 1026, "discrete")
Modbus.RegisterInput("Card Lost Card (disc FC02)", 1027, "discrete")

Modbus.RegisterOutput("Card Out", 1280)
Modbus.RegisterOutput("Card Collect", 1282)
Modbus.RegisterOutput("Card In", 1281)


Modbus.RegisterOutput("Siemen Q0.1", 10001)

-- Modbus.ReadHolding("Sensor Data", 4096)

Log.Info("io_test: registered 4 inputs (2 addresses x 2 spaces) and 3 outputs")

------------------------------------------------------------
-- Connectivity
------------------------------------------------------------

if not Modbus.IsConnected() then
    Log.Error("io_test: PLC is not connected -- check the address on the Configuration page")
    return
end

Log.Info("io_test: PLC connected")

------------------------------------------------------------
-- Which addresses does this PLC actually accept?
------------------------------------------------------------

-- Both spellings exist; ReadDiscInput is an alias of ReadDiscreteInput.
local ReadDI = Modbus.ReadDiscInput or Modbus.ReadDiscreteInput

local okDI, okCoil = {}, {}

for _, base in ipairs(SCAN_BASES) do

    local di = ReadDI(base)
    local co = Modbus.ReadCoil(base)

    -- nil means the read failed (usually exception 0x02 = no such address).
    -- true/false both mean the address exists and was read.
    if di ~= nil then okDI[#okDI + 1] = base .. "=" .. tostring(di) end
    if co ~= nil then okCoil[#okCoil + 1] = base .. "=" .. tostring(co) end

    Log.Info(string.format("io_test: addr %-6d  DiscreteInput=%-6s  Coil=%s",
        base, tostring(di), tostring(co)))
end

if #okDI > 0 then
    Log.Info("io_test: DISCRETE INPUTS readable at -> " .. table.concat(okDI, ", "))
else
    Log.Error("io_test: no discrete inputs readable at any scanned base. " ..
        "The PLC rejected every address (exception 0x02), so its inputs live " ..
        "somewhere else -- check the vendor's address map.")
end

if #okCoil > 0 then
    Log.Info("io_test: COILS readable at -> " .. table.concat(okCoil, ", "))
else
    Log.Warning("io_test: no coils readable at any scanned base either.")
end

------------------------------------------------------------
-- Block read, if anything responded
------------------------------------------------------------

if #okDI > 0 then
    local base = tonumber(string.match(okDI[1], "^(%d+)"))
    local blk = Modbus.ReadDiscInputs(base, 8)
    if blk then
        local bits = ""
        for _, v in ipairs(blk) do bits = bits .. (v and "1" or "0") end
        Log.Info("io_test: ReadDiscInputs(" .. base .. ", 8) = " .. bits)
    else
        Log.Warning("io_test: block read of 8 inputs at " .. base .. " failed " ..
            "(single reads worked, so the PLC likely has fewer than 8 there)")
    end
end

------------------------------------------------------------
-- Idle so the Dashboard panels stay populated
------------------------------------------------------------

Log.Info("io_test: done scanning; idling so the Dashboard keeps polling. Stop when finished.")

while true do
    Sleep(500)
end
