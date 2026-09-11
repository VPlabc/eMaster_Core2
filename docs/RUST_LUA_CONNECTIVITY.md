# Lua connectivity boundary

`emaster-lua::ConnectivityRegistry` is the controlled boundary for Lua-owned
REST, MQTT, and RabbitMQ resources. Lua receives an opaque numeric handle and
must present its application owner for every operation. Handles expose only
kind, name, owner, and lifecycle state; native transport objects remain inside
their managers.

The registry rejects duplicate names within an application, rejects invalid
owners/names, isolates applications from one another, and removes all handles
for an application during unload. `ConnectivityRuntime` now owns the three
Rust managers and dispatches create/start/stop/destroy operations by opaque
handle, while transport-specific operations remain inside those managers.

`ApplicationRuntime` composes the connectivity bridge with the application
lifecycle manager. Stopping or failing an application destroys its owned
connectivity resources; removing it also drops active configuration ownership.
Persisted JSON configuration is retained for explicit cleanup or recovery, and
connection configuration owners must match the creating application.
