# Dynamic application configuration

`emaster-config` now provides the backend contract for application-driven
configuration. A `Schema` declares fields, types, required values, and
defaults. `ApplicationConfigManager` keeps schemas and values keyed by
application ID, validates updates, persists flat JSON objects, reloads them,
and emits lifecycle events through standard channels.

The original string-based `Config` type remains unchanged for system and
driver configuration. Application configuration is intentionally a separate
model so runtime variables and fixed driver settings do not become mixed with
Lua application state.

The JSON reader currently supports the scalar values represented by `Value`.
Nested objects, arrays, and SQLite storage are reserved for a later phase when
the application data model requires them.
