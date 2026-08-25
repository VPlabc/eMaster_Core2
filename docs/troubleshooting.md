# Troubleshooting

Start with the **Logs** page in the web UI — filter by category (Modbus,
Serial, Rest, Lua, Card, Rfid, System) and level. Most of what follows is
diagnosable from there plus the **Test Tools** page.

---

## A PLC input on the dashboard never changes

By far the most common Modbus problem, and it is almost never a wiring fault.

Coils (FC01) and discrete inputs (FC02) are **separate address spaces**. Many
PLCs answer *both* function codes at the same address, but only one carries
the live state — the other returns a constant. Because both replies are valid
Modbus, nothing can detect this automatically.

Each input row on the dashboard shows the function code that produced its
value (`FC01` / `FC02`). If a row is frozen, it is reading the wrong space:

```lua
Modbus.RegisterInput("Card Taken", 1024, "coil")      -- FC01
Modbus.RegisterInput("Door Sensor", 1025, "discrete") -- FC02
```

To find out which space your PLC uses, run `config/scripts/io_test.lua`. It
registers each address under *both* function codes side by side — trigger the
sensor and see which row moves.

## A register value is nonsense (2.7e+23, 1.4e-45, garbled text)

Wrong byte or word order, not bad data. Modbus moves 16-bit words; how a
32/64-bit value is laid across them is a convention between the PLC program
and whoever reads it.

The **Raw** column on the PLC Registers card shows the underlying words.
Compare against the four orders for `0x12345678`:

| `endian` | Register 0 | Register 1 |
|---|---|---|
| `ABCD` | `0x1234` | `0x5678` |
| `BADC` | `0x3412` | `0x7856` |
| `CDAB` | `0x5678` | `0x1234` |
| `DCBA` | `0x7856` | `0x3412` |

```lua
Modbus.RegisterRegister("Temperature", 4096,
    { type = "float32", endian = "CDAB" })
```

Registering the same address twice under two orders shows both at once.

## "Modbus exception 0x2 (illegal data address)"

The address is not mapped on that PLC. Use **Test Tools → Modbus TCP Client →
Scan common base addresses** to sweep 0, 1, 100, 1000, 1024, 2048, 4096, 8192,
10000 and 10001 with both FC01 and FC02.

Exceptions do *not* count toward the disconnect threshold — a device that
answers is alive — so probing bad addresses will not drop a healthy link.

## PLC shows disconnected, or the dashboard stalls briefly

The gateway retries every 5 seconds. Check with **Test Tools → Modbus TCP
Client → Connect only**.

Known issue: `ModbusClient` holds its mutex across the TCP connect, so
`/api/status` can block for up to the connect timeout while a reconnect to an
unreachable PLC is in flight. A dashboard that freezes for a second or two
every 5 seconds, with the PLC down, is this.

---

## Serial port will not open

**Linux/macOS** — permissions, usually:

```bash
sudo usermod -aG dialout $USER      # uucp on some distributions
# log out and back in
ls -l /dev/ttyUSB0
```

**Windows** — confirm the port number in Device Manager; `COM10` and above are
fine.

Either platform: only one process may hold a port. The gateway itself holds
Serial1, and Serial2 once `led.lua` opens it, so **Test Tools → Serial Master
will fail to open those** — that is expected, not a fault. Close any terminal
emulator too.

Find ports with **Test Tools → Serial Master → Scan**, or the Scan button on
the Configuration page.

## LED display shows nothing

Serial2 is opened by `led.lua`, not at startup, so a wrong port does not
appear until a script runs. Use **Configuration → Serial Port 2 → Send test
frame**, which writes a real TDM-800 frame without needing a script.

The panel is ASCII-only and a CR terminates the frame; both are handled, but
custom frames sent from a script must respect them.

---

## `zk.*` calls fail with `NotSupportedOnThisPlatform`

Your build has no ZKTeco support. Confirm:

```bash
./bin/hsf_gateway --version     # last line: "ZK controller: disabled"
```

The PullSDK is a 32-bit Windows DLL. Use the **windows-x86** package; there is
no equivalent for Linux, macOS or Windows x64.

## ZK card value does not match the number on the card

RTLog carries only the 24 Wiegand data bits. The full value is
`(raw << 1) | oddParity(low 12 bits)` — see `config/scripts/zkcard.lua`.

---

## A Lua script fails with "module 'x' not found"

`require()` searches the script's own directory first, then the scripts root.
A module in a sibling folder is not on either path — move it, or require it by
its path relative to the root.

## Registrations disappeared

Starting *any* script clears all `Modbus.Register*` registrations — they
describe the running script. Declare and use points in the same script; do not
declare them in one run and read them in another.

## A script shows ERROR after Stop

Fixed in 1.0.0 — a normal interrupt is now reported as `STOPPED`. If you still
see it, check the message: a genuine runtime error also stops the script.

## Vietnamese text loses its diacritics

Lua's `string.upper` is ASCII-only and strips them. `config/scripts/utils.lua`
handles both cases explicitly; use that rather than `string.upper` directly.

---

## Cannot reach the dashboard

- `bind_address` of `127.0.0.1` accepts local connections only; use `0.0.0.0`
  for remote access.
- Open the port in the firewall (`8080` by default).
- Confirm the gateway started: it logs `eMaster Gateway starting`.

## Dashboard loads but nothing updates

Values arrive over a WebSocket at `/ws`. If the page is static, look for
WebSocket errors in the browser console — a proxy that does not forward the
upgrade header is the usual cause.

## Configuration changes do not stick

`config.db` is authoritative once created; `config.json` is imported **only**
when creating a new database. Editing `config.json` afterwards does nothing —
use the Configuration page, or delete `config.db` to re-import (which discards
all current settings, including the REST API key).

## Anyone can reach Admin pages

Known limitation. Role restrictions are enforced in the frontend only; the
REST endpoints do not check them. Do not expose the dashboard to an untrusted
network.

---

## Reporting a problem

Include the output of:

```bash
./bin/hsf_gateway --version
```

or the **System Info** tab, plus the relevant slice of the Logs page
(**Download** exports what the current filters show).
