# Application configuration UI

The configuration page keeps fixed system/driver settings in the existing
form and renders application settings in a separate Applications section.

The renderer consumes `GET /api/application-config`, accepting either an
array or `{ "applications": [] }`. Each application provides an application
ID, optional title, schema fields, and current values. Fields are created from
schema metadata rather than application-specific HTML. Supported browser
controls include strings, URLs, passwords, numbers, integers, ports, booleans,
textareas, and JSON textareas.

Saving posts `{ application, values }` to `POST /api/application-config`.
If the endpoint is unavailable, the existing system configuration page remains
fully usable and the application section stays hidden.
