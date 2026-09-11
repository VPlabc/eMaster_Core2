# Configuration sections

The web configuration page now exposes three explicit navigation anchors:

- **System** for machine identity and system-level settings.
- **Drivers** for built-in Lua startup, REST, serial, Modbus, ZK, and message
  transport settings.
- **Applications** for schema-generated settings supplied by Lua applications
  and plugins.

Existing field names, persistence, and save behavior are unchanged. The
section boundaries are semantic and provide stable targets for future
sidebar/tab navigation while keeping application cards independent from the
fixed driver form.
