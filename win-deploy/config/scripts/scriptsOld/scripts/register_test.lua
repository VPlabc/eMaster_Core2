-- register_test.lua
-- Declares one register point per data type / endianness so the Dashboard's
-- "PLC Registers" card can be checked against a known layout.
--
-- Modbus moves 16-bit words and nothing else. Anything wider than 16 bits is
-- a convention between the PLC program and whoever reads it, which is why
-- every point below states its type AND its byte/word order explicitly.
--
--   FC03  holding registers  read/write   (written with FC06 / FC16)
--   FC04  input registers    READ-ONLY    (Modbus defines no FC04 write)
--
-- Endianness, using the four-letter names PLC manuals use. For the 32-bit
-- value 0x12345678 stored across two registers:
--
--   ABCD  big endian     R0=0x1234 R1=0x5678   (default)
--   BADC  byte swap      R0=0x3412 R1=0x7856
--   CDAB  word swap      R0=0x5678 R1=0x1234
--   DCBA  little endian  R0=0x7856 R1=0x3412
--
-- If a value reads as nonsense, compare the Raw column on the dashboard
-- against these four -- the bytes are right, the order is the question.

local BASE = 4096

------------------------------------------------------------
-- Integers, holding registers (writable)
------------------------------------------------------------

Modbus.RegisterRegister("U16 Counter", BASE + 0,
    { type = "uint16", write = true, unit = "counts" })

Modbus.RegisterRegister("I16 Setpoint", BASE + 1,
    { type = "int16", write = true, unit = "degC" })

Modbus.RegisterRegister("U32 Total", BASE + 2,
    { type = "uint32", endian = "ABCD", write = true })

Modbus.RegisterRegister("I32 Offset", BASE + 4,
    { type = "int32", endian = "CDAB", write = true })

Modbus.RegisterRegister("U64 Serial", BASE + 6,
    { type = "uint64", endian = "ABCD" })

Modbus.RegisterRegister("I64 Ticks", BASE + 10,
    { type = "int64", endian = "DCBA" })

------------------------------------------------------------
-- Floats -- the usual reason a value looks wrong
------------------------------------------------------------

Modbus.RegisterRegister("F32 Temp (ABCD)", BASE + 14,
    { type = "float32", endian = "ABCD", write = true, unit = "degC" })

Modbus.RegisterRegister("F32 Temp (CDAB)", BASE + 14,
    { type = "float32", endian = "CDAB", unit = "degC" })

Modbus.RegisterRegister("F64 Pressure", BASE + 16,
    { type = "float64", endian = "ABCD", write = true, unit = "kPa" })

------------------------------------------------------------
-- String -- length is in BYTES, two per register
------------------------------------------------------------

Modbus.RegisterRegister("Device Tag", BASE + 20,
    { type = "string", length = 16, write = true })

Modbus.RegisterRegister("Batch ID (byte swapped)", BASE + 28,
    { type = "string", length = 8, endian = "BADC" })

------------------------------------------------------------
-- Input registers (FC04) -- read-only by protocol.
-- Passing write = true here raises an error rather than being ignored.
------------------------------------------------------------

Modbus.RegisterRegister("Live Current", 3000,
    { type = "float32", source = "input", endian = "ABCD", unit = "A" })

Modbus.RegisterRegister("Uptime Seconds", 3002,
    { type = "uint32", source = "input" })

Log.Info("register_test: declared 13 register points")

------------------------------------------------------------
-- Reading and writing the same points from Lua
------------------------------------------------------------

-- Modbus.GetRegister(name) -> value, error
-- Modbus.SetRegister(name, value) -> ok, error
--
-- Both use the declared type and endianness, so a script never re-implements
-- the word packing:
--
--   local temp, err = Modbus.GetRegister("F32 Temp (ABCD)")
--   if temp then Log.Info("temp = " .. temp) else Log.Warning(err) end
--
--   local ok, err = Modbus.SetRegister("I16 Setpoint", -120)
--   if not ok then Log.Warning("setpoint write failed: " .. err) end
