# Rust MQTT foundation

`emaster-mqtt::MqttManager` provides the second connectivity migration phase.
It owns independent broker resources by connection ID and application owner.

The manager supports:

- create, start, stop, restart, destroy, status, and list
- owner-scoped cleanup for application unload
- publish with QoS and retained-message metadata
- multiple subscriptions per broker
- subscription restoration when a connection restarts
- payload and topic validation
- connection statistics and last-error tracking

`MqttTransport` is an injected boundary. The default transport fails closed,
so the new Rust API cannot accidentally create network activity before a real
MQTT client is selected. Existing C++ MQTT behavior remains the production
reference during this migration.
