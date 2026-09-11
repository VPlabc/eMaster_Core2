# Production transport rollout

The selected production clients are:

- MQTT: `rumqttc`.
- RabbitMQ: `lapin` and Tokio.

The default workspace build continues to use injected transports, which keeps
unit tests deterministic and avoids silently adding network activity. Both
crates include feature-gated production adapters and ignored broker tests.

MQTT sessions are keyed by configured connection name rather than broker
client ID. This prevents disconnecting one independently managed resource from
removing another resource that happens to use the same client ID.

Broker integration tests must still cover TLS, authentication, reconnect,
subscription/consumer recovery, and shutdown before production enablement.
