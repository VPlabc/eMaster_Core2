# Lua Dynamic Configuration

Lua Edge Logic projects can expose a schema to the generic configuration
manager by adding `config_schema.lua` beside the project entry script. The
file returns one Lua table and must not contain runtime values or business
logic.

```lua
return {
    version = 1,
    title = "My Project",
    fields = {
        { key = "enabled", label = "Enabled", type = "boolean", default = true },
        { key = "timeout", label = "Timeout", type = "number", default = 5000, min = 100, max = 30000 },
        { key = "token", label = "Token", type = "password" },
    },
}
```

Schemas may use a flat `fields` array or `groups`, whose entries contain their
own `fields` arrays. Supported field types are `string`, `password`, `number`,
`boolean`, `select`, `textarea`, and `url`. Number fields may specify `min`,
`max`, `step`, and `unit`; select fields must provide `{ value, label }`
options.

Schema metadata and runtime values are separate. Lua business logic should use
the project-scoped dynamic config API (`get`, `set`, `get_all`, `set_all`) and
must never read the persistence database directly. Updates are validated as a
whole before persistence or runtime application.

Password fields are secrets: they are masked in read responses, are not logged,
and a masked value submitted unchanged means “keep the current value”.

Schema versions are integers. Increment the version when the shape or meaning
of a field changes and provide a migration hook before changing stored values.

The two reference projects are:

- `win-deploy/config/scripts/CardDispenser/config_schema.lua`
- `win-deploy/config/scripts/smartlocker/config_schema.lua`
